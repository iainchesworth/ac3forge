# Coding tools

Expert tier. The tab is always in Expert's tab bar, but the tools themselves exist in the E-AC-3
syntax only: with AC-3 selected, the controls give way to an explainer card — *"AC-3 has no
Annex E tools — coupling bands, spectral extension and AHT exist in the E-AC-3 syntax only.
Switch the codec to Dolby Digital Plus on the Format tab and they appear here."* — and object
mode shows its own equivalent (the JOC bed is coded with the encoder's own fixed tool choices;
turning object mode off on the Objects tab brings the controls back).

Under E-AC-3, three checkboxes, all off by default:

![Coding tools tab, nothing enabled](screenshots/coding-tools-off.png)

> Each of these buys bits somewhere and spends quality somewhere else, so none is on by default.
> Encoding the same material with and without one is the only way to say whether it earned its
> place.

Checking a tool reveals a spin box next to it that reads **auto** at its lowest value — leave it
there to let the encoder pick a band edge, or pin one explicitly:

![Channel coupling and spectral extension both on, band edges on auto, seam attenuation checkbox revealed](screenshots/coding-tools-enabled.png)

| Tool | What its spin box means |
|---|---|
| **Channel coupling** | begin band — auto or pinned (§7.4 / §E3.3) |
| **Spectral extension** | begin band — auto or pinned (§E3.6). Turning it on reveals a further **Attenuate the spectral-extension seam** checkbox. |
| **Adaptive hybrid transform** | GAQ mode, 0–3 — mode 0 is AHT with gain-adaptive quantization switched off, which is how GAQ's own contribution gets measured (§E3.4) |

At the foot of the card, a monospace `ac3cli tools token` readout (`cpl+spx`, `cpl+spx+aht`, …)
shows the exact string that reproduces this configuration on the command line — see
[CLI → Options & grammars](../cli/metadata-options.md) for the full `tools:` grammar, including the
`cpl:N`/`spx:N`/`aht:N` pinning syntax this token expands to.

## Next

[Metadata](metadata.md) — loudness, downmix, and heavy compression, the rest of what Expert
adds.
