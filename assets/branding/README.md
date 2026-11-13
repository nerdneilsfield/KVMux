# KVMux artwork

The editable SVG files are a manual redraw of the layered-screen mark and orange
connection in the user-provided raster design board, `ChatGPT Image 2026年9月12日
20_09_19.png` (1672 × 941). They are not the original vector artwork. The redraw
preserves the screen arrangement and connection, with simplified gradients and
clean edges. The dark application tile follows the board’s icon variant. No
trademark symbol or claim has been added.

| File | Use | Size |
| --- | --- | --- |
| `logo.png` / `logo.svg` | Transparent standalone mark | 1024 × 1024 |
| `app-icon.png` / `app-icon.svg` | Dark rounded application tile | 1024 × 1024 |
| `banner.png` / `banner.svg` | README header on light or dark pages | 1600 × 480 |
| `app.icns` | macOS bundle icon | 16–1024 px representations |
| `app.ico` | Windows executable icon | 16, 24, 32, 48, 64, 128, 256 px |

## Palette

- Midnight: `#0B1823`
- Charcoal: `#202A33`
- Silver: `#BFC5C9`
- Warm white: `#F3F1EB`
- Connection orange: `#C45F3C`; highlight: `#ED9464`

These are chosen clean equivalents of the raster’s shaded colors, not recovered
original brand specifications. The banner uses Helvetica/Arial with a sans-serif
fallback; its committed PNG fixes the rendered appearance across README viewers.

The interface uses the same colors through `src/app/ui_theme.hpp`, plus these
extensions:

- Video well and the dark end of the logo gradient: `#08121B`
- Border: `#576570`
- Raised inputs (`surface_raised`): `#2C3842`
- Primary button fill: `#633A2E`; hover: `#84442E`. The active fill is the same
  `#633A2E`. Button text is warm white. Connection orange is not a button fill.
- Status: ok `#3DDC97`, warn `#E6C84A`, error `#FF6B6B`. Warn is yellow so it
  stays distinct from connection orange. An error banner is a charcoal surface
  with a left error bar, not a solid status-color fill.
- Window and popup borders use Silver at 40% opacity.

## Export

PNG files were rendered directly from the SVGs at their declared dimensions with
CairoSVG. ICNS and ICO were exported from `app-icon.png` with Pillow. These are
artwork authoring tools only, not build or runtime dependencies. Do not upscale a
small crop from the design board. On Apple Silicon with Homebrew Cairo installed:

```sh
DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib uv run --with cairosvg --with pillow assets/branding/export.py
```

CMake includes the ICNS in the macOS bundle and the ICO in a Windows resource.
Linux installs the PNG into the hicolor icon theme and a desktop entry. The GUI
loads `assets/branding/logo.png` relative to `SDL_GetBasePath()`; on macOS this is
`Contents/Resources`, while other platforms use the executable directory.
