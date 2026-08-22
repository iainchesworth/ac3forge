#!/usr/bin/env python3
"""Generates every ac3forge app-icon asset from one procedural mark.

Single source of truth: the mark is drawn by draw_badge()/draw_bars()
below, not edited by hand in any of the .ico/.icns/.png/mipmap files this
writes. Re-run this script (`python assets/icon/generate_icons.py`) after
changing the design, rather than touching a generated output directly.
Mirrors the "commit the binary asset into the tree" precedent
apps/gui/fonts/*.ttf already sets for this project - just generated here
rather than third-party.

Pillow only (confirmed available: 12.3+, including native ICO and ICNS
write support - no Inkscape/rsvg-convert/ImageMagick/iconutil needed,
none of which are installed in this environment). The one place a raster
doesn't fit - the WASM favicon wants to stay crisp at arbitrary sizes -
is covered instead by the hand-authored assets/icon/ac3forge-icon.svg,
drawn to match this script's output rather than generated from it.
"""

from __future__ import annotations

import pathlib

from PIL import Image, ImageDraw

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

# The WASM demo page's own existing palette (apps/wasm/index.html's
# :root CSS vars: --bg/--accent) - reused here rather than inventing a
# third brand color for a project that already has one established.
BG = (11, 18, 32, 255)  # #0b1220
ACCENT = (96, 165, 250, 255)  # #60a5fa

# Rendered once at high resolution and downsampled (LANCZOS) for every
# smaller output below, so even the 16px Windows .ico frame is
# anti-aliased from real detail rather than drawn crudely at 16px.
SUPERSAMPLE = 1024


def draw_bars(size: int, *, content_frac: float) -> Image.Image:
    """Just the 3-bar equalizer/waveform glyph, transparent background.

    Three bars, not five or more: a busier glyph blurs into a smudge at
    16px, where three symmetric bars (0.55, 1.0, 0.55 of the content
    height - a single waveform "pulse") still read clearly. Used both
    standalone (composited onto draw_badge's rounded-rect background) and
    alone (the Android adaptive-icon foreground layer, which supplies its
    own flat-color background separately - see ic_launcher_background.xml).
    """
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    content = size * content_frac
    origin = (size - content) / 2
    heights = (0.55, 1.0, 0.55)
    gap_frac = 0.28  # gap width as a fraction of one bar's own width
    bar_w = content / (len(heights) + (len(heights) - 1) * gap_frac)
    gap_w = bar_w * gap_frac
    radius = bar_w / 2

    for i, h in enumerate(heights):
        bar_h = content * h
        x0 = origin + i * (bar_w + gap_w)
        x1 = x0 + bar_w
        y1 = origin + content - (content - bar_h) / 2
        y0 = y1 - bar_h
        draw.rounded_rectangle([x0, y0, x1, y1], radius=radius, fill=ACCENT)

    return img


def draw_badge(size: int, *, corner_frac: float, content_frac: float) -> Image.Image:
    """The full mark: a rounded-square (or, at corner_frac=0.5, circular)
    badge in BG with draw_bars() centered on top."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    draw.rounded_rectangle(
        [0, 0, size - 1, size - 1], radius=int(size * corner_frac), fill=BG
    )
    img.alpha_composite(draw_bars(size, content_frac=content_frac))
    return img


def render_badge(
    size: int, *, corner_frac: float = 0.22, content_frac: float = 0.62
) -> Image.Image:
    hi = draw_badge(SUPERSAMPLE, corner_frac=corner_frac, content_frac=content_frac)
    return hi if size == SUPERSAMPLE else hi.resize((size, size), Image.LANCZOS)


def render_round_badge(size: int, *, content_frac: float = 0.58) -> Image.Image:
    # corner_frac=0.5 on a square rounded-rect clips to a full circle -
    # ic_launcher_round's expected shape for launchers that don't apply
    # their own adaptive-icon circular mask.
    hi = draw_badge(SUPERSAMPLE, corner_frac=0.5, content_frac=content_frac)
    return hi if size == SUPERSAMPLE else hi.resize((size, size), Image.LANCZOS)


def render_foreground(size: int, *, content_frac: float = 0.46) -> Image.Image:
    # Adaptive icons only guarantee the inner ~66%-diameter circle (the
    # "safe zone") survives the launcher's own mask/parallax animation -
    # smaller content_frac than the standalone badge so the bars stay
    # well inside that zone. Transparent background: ic_launcher_background
    # (a flat color resource) is composited underneath by Android itself,
    # per <adaptive-icon> in ic_launcher.xml/ic_launcher_round.xml.
    hi = draw_bars(SUPERSAMPLE, content_frac=content_frac)
    return hi if size == SUPERSAMPLE else hi.resize((size, size), Image.LANCZOS)


def render_banner(width: int = 320, height: int = 180) -> Image.Image:
    """The Android-TV/Leanback launcher-row banner: the mark centered on
    a wordmark-free flat background. A mark-plus-text lockup was tried
    first, but "ac3forge" set in the GUI's own Archivo typeface does not
    fit this banner's tight 320x180 aspect ratio without truncating -
    the mark alone is what every reference Leanback banner example uses
    for exactly this reason. Without a banner at all, Shield's TV
    home-screen tile falls back to a poorly-scaled launcher icon instead
    of this purpose-built shape."""
    img = Image.new("RGBA", (width, height), BG)
    mark_size = int(height * 0.78)
    mark = render_badge(mark_size, corner_frac=0.24, content_frac=0.6)
    offset = ((width - mark_size) // 2, (height - mark_size) // 2)
    img.alpha_composite(mark, offset)
    return img


def write_ico(path: pathlib.Path) -> None:
    sizes = (16, 32, 48, 256)
    frames = [render_badge(s) for s in sizes]
    frames[-1].save(
        path,
        format="ICO",
        sizes=[(s, s) for s in sizes],
        append_images=frames[:-1],
    )


def write_icns(path: pathlib.Path) -> None:
    # Pillow's ICNS writer is pure Python (no iconutil/macOS host needed):
    # give it one large square source image and it derives every OSType
    # entry (ic07..ic10 etc.) itself.
    render_badge(SUPERSAMPLE).save(path, format="ICNS")


# (density bucket, launcher icon px)
ANDROID_DENSITIES = (
    ("mdpi", 48),
    ("hdpi", 72),
    ("xhdpi", 96),
    ("xxhdpi", 144),
    ("xxxhdpi", 192),
)


def main() -> None:
    icons_dir = REPO_ROOT / "apps" / "gui" / "icons"
    icons_dir.mkdir(parents=True, exist_ok=True)

    write_ico(icons_dir / "ac3forge.ico")
    write_icns(icons_dir / "ac3forge.icns")
    render_badge(32).save(icons_dir / "ac3forge-32.png")
    render_badge(256).save(icons_dir / "ac3forge-256.png")
    print(f"wrote {icons_dir}/ac3forge.{{ico,icns}}, ac3forge-{{32,256}}.png")

    res_dir = REPO_ROOT / "apps" / "android" / "app" / "src" / "main" / "res"

    # Legacy launcher icon, every density bucket - the fallback for
    # launchers/contexts (app-list entries, Settings) that don't use the
    # adaptive-icon layers below.
    for density, px in ANDROID_DENSITIES:
        mipmap_dir = res_dir / f"mipmap-{density}"
        mipmap_dir.mkdir(parents=True, exist_ok=True)
        render_badge(px).save(mipmap_dir / "ic_launcher.png")
        render_round_badge(px).save(mipmap_dir / "ic_launcher_round.png")
    print(f"wrote {res_dir}/mipmap-*/ic_launcher{{,_round}}.png (5 densities)")

    # Adaptive-icon foreground (API 26+). One density bucket only, same
    # single-device-family simplification build.gradle.kts already makes
    # for abiFilters (Shield TV hardware only) - Android falls back to this
    # bucket for any density it's asked to render at.
    fg_dir = res_dir / "mipmap-xxxhdpi"
    fg_dir.mkdir(parents=True, exist_ok=True)
    render_foreground(432).save(fg_dir / "ic_launcher_foreground.png")
    print(f"wrote {fg_dir}/ic_launcher_foreground.png")

    banner_dir = res_dir / "drawable"
    banner_dir.mkdir(parents=True, exist_ok=True)
    render_banner().save(banner_dir / "banner.png")
    print(f"wrote {banner_dir}/banner.png")

    wasm_dir = REPO_ROOT / "apps" / "wasm"
    render_badge(32).save(wasm_dir / "favicon-32.png")
    print(f"wrote {wasm_dir}/favicon-32.png")


if __name__ == "__main__":
    main()
