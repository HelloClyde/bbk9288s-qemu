#!/usr/bin/env python3

from pathlib import Path

from PIL import Image, ImageDraw, ImageOps


CANVAS_SIZE = (1600, 900)
TILE_SIZE = (748, 421)
BACKGROUND = "#e8ecee"
BORDER = "#9da6a9"
POSITIONS = (
    (44, 21),
    (808, 21),
    (44, 458),
    (808, 458),
)
SOURCES = (
    "sanguo-map-web.png",
    "sanguo-title-web.png",
    "sanguo-city-web.png",
    "web-emulator.png",
)


def main() -> None:
    assets = Path(__file__).resolve().parents[1] / "docs" / "assets"
    canvas = Image.new("RGB", CANVAS_SIZE, BACKGROUND)
    draw = ImageDraw.Draw(canvas)

    for source_name, position in zip(SOURCES, POSITIONS, strict=True):
        with Image.open(assets / source_name) as source:
            tile = ImageOps.fit(
                source.convert("RGB"),
                TILE_SIZE,
                method=Image.Resampling.LANCZOS,
            )
        canvas.paste(tile, position)
        x, y = position
        draw.rectangle(
            (x, y, x + TILE_SIZE[0] - 1, y + TILE_SIZE[1] - 1),
            outline=BORDER,
            width=1,
        )

    output = assets / "preview.png"
    canvas.save(output, optimize=True)
    print(output)


if __name__ == "__main__":
    main()
