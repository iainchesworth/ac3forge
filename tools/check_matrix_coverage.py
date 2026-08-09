"""Does scripts/run-codec-matrix.sh actually mention every option the CLI has?

Every other check in this project asks "is the output correct". This one asks
a different question: "does anything exercise this at all". A new layout, a
new Annex E tool token, or a whole new command can land in the CLI without
anyone remembering to add it to the FFmpeg-oracle matrix - the matrix would
keep passing, green and silent, having never touched the new code path.

So this reads the CLI's own canonical option lists - not a second,
hand-maintained copy of them, which could drift from the real one exactly the
way the matrix itself can - and checks each token appears somewhere in
scripts/run-codec-matrix.sh. Three of the four checks below ask the binary
directly, via the same error-message path real bad input hits:

  commands   ac3cli with no args prints its own usage table.
  layouts    ac3cli sine/eac3-sine reject a bogus layout with
             "unknown layout 'X' (mono | stereo | ...)" - built from
             ac3::plan::layout_names(codec), the same function 'sine' and
             'eac3-sine' validate a real layout against.
  tools      ac3cli eac3-encode rejects a bogus tool set with
             "unknown tool set 'X' (none | cpl | ...)" - built from
             ac3::plan::kToolsSyntax. This one IS a hand-maintained string
             (see plan.hpp), not derived from parse_tools()'s own token
             list, so it carries its own smaller drift risk one level
             removed from what this script checks - out of scope here.
  atmos mode objects/bed51 has no equivalent introspection (atmos's mode
             argument fails silently to whatever run_atmos's own validation
             does, not through a listable error), so this one stays a small
             hardcoded pair with a comment saying why, checked with the same
             presence test as everything else.
  vbr        eac3-encode's [vbr] argument is not a fixed enum - "off" or a
             continuous quality float plus optional min:/max: bounds
             ("q:0..1[,min:kbps][,max:kbps]") - so there is no token list to
             diff the way layouts/tools have. This just checks that SOME
             quality-mode encode (a literal "q:" token) appears in the
             matrix at all. First added after VBR support landed in the
             library with zero matrix coverage - the exact gap this whole
             script exists to catch - and only the CLI's own usage text
             said the [vbr] argument existed at all.

Coverage itself is a presence check, not a scoped one: each canonical token
just has to appear as a whole word somewhere in the matrix script. That is
looser than confirming it is in the *right* loop, but it is what makes this
check itself hard to fool by accident, and false negatives (a token that
"looks" covered but isn't really exercised) are far cheaper here than false
positives (a real gap that stays invisible) - see CONTRIBUTING.md's
validation discipline.

Usage (repo root, after building):
  python tools/check_matrix_coverage.py [--cli path] [--matrix path]
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

FAILURES: list[str] = []

# Commands this matrix cannot reasonably drive headlessly: real capture/
# playback hardware (record, live, devices, outputs, play, monitor) or the
# CLI's own meta-flag (--version). Anything else that writes a stream is
# expected to appear in the matrix.
EXCLUDED_COMMANDS = {
    "--version", "record", "live", "devices", "outputs", "play", "monitor",
}

# No CLI introspection exists for this one - see the module docstring.
ATMOS_MODES = {"objects", "bed51"}


def run(cli: str, *args: str) -> tuple[int, str, str]:
    result = subprocess.run([cli, *args], capture_output=True, text=True)
    return result.returncode, result.stdout, result.stderr


def usage_commands(cli: str) -> set[str]:
    _, out, _ = run(cli)
    names = set()
    for line in out.splitlines():
        m = re.match(r"\s*ac3cli\s+(\S+)", line)
        if m:
            names.add(m.group(1))
    if not names:
        raise SystemExit("could not parse any commands from `ac3cli`'s usage output")
    return names


def vbr_supported(cli: str) -> bool:
    """Whether this build's eac3-encode has a [vbr] argument at all - lets
    the vbr check skip cleanly on an older build instead of asserting
    something that would not even parse."""
    _, out, _ = run(cli)
    for line in out.splitlines():
        if re.match(r"\s*ac3cli\s+eac3-encode\b", line) and "[vbr]" in line:
            return True
    return False


def layout_names(cli: str, tmp: Path, command: str) -> set[str]:
    probe = tmp / "layout_probe.out"
    _, _, err = run(cli, command, str(probe), "1", "192", "1000", "50",
                     "__coverage_probe__")
    m = re.search(r"unknown layout '.*?' \((.*)\)\s*$", err.strip())
    if not m:
        raise SystemExit(f"could not parse a layout list from `{command}`'s error output:\n{err}")
    return {token.strip() for token in m.group(1).split("|")}


def tool_names(cli: str, tmp: Path) -> set[str]:
    # eac3-encode needs a real WAV before it gets far enough to validate the
    # tool token, so bootstrap one from 'silence' - content does not matter,
    # only that the file exists and has the right shape.
    wav = tmp / "tool_probe.wav"
    ac3 = tmp / "tool_probe.ac3"
    run(cli, "silence", str(ac3), "1", "192")
    run(cli, "decode", str(ac3), str(wav))
    probe = tmp / "tool_probe.ec3"
    _, _, err = run(cli, "eac3-encode", str(wav), str(probe), "192",
                     "__coverage_probe__", "51")
    # The captured group is plan::kToolsSyntax verbatim, e.g.
    # "none | cpl | spx | aht | all (cpl:N / spx:N pin a band edge, ...)" -
    # greedy-matching to the LAST ')' on the line grabs the whole thing,
    # nested parenthetical included; stripping each split token at its own
    # first '(' then discards that explanatory tail per-token.
    m = re.search(r"unknown tool set '.*?' \((.*)\)\s*$", err.strip())
    if not m:
        raise SystemExit(f"could not parse a tool list from eac3-encode's error output:\n{err}")
    tokens = set()
    for token in m.group(1).split("|"):
        tokens.add(token.split("(")[0].strip())
    return tokens


def covered(matrix_text: str, token: str) -> bool:
    return re.search(rf"\b{re.escape(token)}\b", matrix_text) is not None


def commands_invoked(matrix_text: str) -> set[str]:
    return set(re.findall(
        r"\brun(?:_tolerate_eac3_tool_unsupported)?\s+([a-z][a-z0-9-]*)", matrix_text))


def check(name: str, canonical: set[str], missing: set[str]) -> None:
    ok = not missing
    if ok:
        detail = f"{len(canonical)} covered: {sorted(canonical)}"
    else:
        detail = f"missing {sorted(missing)} (of {sorted(canonical)})"
    print(f"  {'PASS' if ok else 'FAIL'}  {name}: {detail}")
    if not ok:
        FAILURES.append(f"{name} ({', '.join(sorted(missing))})")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cli", default="build/dev/bin/ac3cli.exe")
    parser.add_argument("--matrix", default="scripts/run-codec-matrix.sh")
    args = parser.parse_args()

    cli = str((REPO / args.cli).resolve() if not Path(args.cli).is_absolute() else args.cli)
    if not Path(cli).exists():
        raise SystemExit(f"ac3cli not found at {cli} - build first, or pass --cli")
    matrix_path = REPO / args.matrix if not Path(args.matrix).is_absolute() else Path(args.matrix)
    matrix_text = matrix_path.read_text()

    with tempfile.TemporaryDirectory(prefix="ac3matrixcov_") as tmp_str:
        tmp = Path(tmp_str)

        print("commands - every stream-producing ac3cli command the matrix should invoke")
        canonical_commands = usage_commands(cli) - EXCLUDED_COMMANDS
        invoked = commands_invoked(matrix_text)
        check("commands", canonical_commands, canonical_commands - invoked)

        print("AC-3 layouts - ac3cli sine's own accepted set")
        ac3_layouts = layout_names(cli, tmp, "sine")
        missing = {t for t in ac3_layouts if not covered(matrix_text, t)}
        check("ac3-layouts", ac3_layouts, missing)

        print("E-AC-3 layouts - ac3cli eac3-sine's own accepted set")
        eac3_layouts = layout_names(cli, tmp, "eac3-sine")
        missing = {t for t in eac3_layouts if not covered(matrix_text, t)}
        check("eac3-layouts", eac3_layouts, missing)

        print("Annex E tools - ac3cli eac3-encode's own accepted set (minus 'none')")
        tools = tool_names(cli, tmp) - {"none"}
        missing = {t for t in tools if not covered(matrix_text, t)}
        check("annex-e-tools", tools, missing)

        print("Atmos modes - no CLI introspection; small hardcoded set, see module docstring")
        missing = {t for t in ATMOS_MODES if not covered(matrix_text, t)}
        check("atmos-modes", ATMOS_MODES, missing)

        print("E-AC-3 VBR - eac3-encode's [vbr] quality-rate mode, not a fixed enum")
        if vbr_supported(cli):
            has_vbr = re.search(r"\bq:[0-9]", matrix_text) is not None
            check("eac3-vbr", {"q:<quality>"}, set() if has_vbr else {"q:<quality>"})
        else:
            print("  SKIP  eac3-vbr: this ac3cli build has no [vbr] argument")

    print()
    if FAILURES:
        print(f"{len(FAILURES)} coverage gap(s): {'; '.join(FAILURES)}")
        print("Add the missing option to scripts/run-codec-matrix.sh (or, if it "
              "genuinely cannot run headlessly, add it to this script's exclusion set "
              "with a reason).")
        sys.exit(1)
    print("matrix covers every option the CLI currently exposes")


if __name__ == "__main__":
    main()
