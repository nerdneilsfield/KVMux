"""Export committed branding images; requires authoring-only CairoSVG and Pillow."""

from pathlib import Path

import cairosvg
from PIL import Image


root = Path(__file__).resolve().parent
for name in ("logo", "app-icon", "banner"):
    cairosvg.svg2png(url=str(root / f"{name}.svg"), write_to=str(root / f"{name}.png"))

with Image.open(root / "app-icon.png") as icon:
    icon.save(root / "app.ico", sizes=[(size, size) for size in (16, 24, 32, 48, 64, 128, 256)])
    icon.save(root / "app.icns", sizes=[(size, size) for size in (16, 32, 64, 128, 256, 512, 1024)])

for name in ("logo.png", "app-icon.png", "banner.png", "app.ico", "app.icns"):
    with Image.open(root / name) as image:
        print(name, image.size, image.format)
