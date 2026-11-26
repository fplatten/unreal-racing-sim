"""
Paints the Formula cars' sponsor lettering: SourceArt/Generated/decals_mask.png, a 1024 x 2048 atlas
of 16 rows (1024 x 128 each), white lettering on black, centered in its row. Rows 0-9 are long names
(up to 5.8:1), rows 10-15 short ones (up to 2.6:1) for the narrow panels. The names are made up; some
are the trackside boards' (Scripts/BakeTextures.java).

  python Scripts/make_decals.py

M_Livery (Scripts/livery.hlsl) projects a row onto each panel of the car and colors it.
"""
import os

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "SourceArt", "Generated", "decals_mask.png"))

W, ROW, ROWS = 1024, 128, 16
LONG_MAX, SHORT_MAX = 740, 330   # widest lettering (px) for the long and the short rows
CAP = 92                          # letter height (px)

FONTS = ["impact.ttf", "arialbi.ttf", "segoeuiz.ttf", "bahnschrift.ttf", "arialbd.ttf"]
LONG = ["NOVA FUEL", "VELOCE", "STRATOS", "MERIDIAN", "HELIX OIL", "ORBITA TELECOM", "SOLARA",
        "VANGUARD", "CASCADE", "ZEPHYR RACING"]
SHORT = ["APEX", "TORQUE", "NOVA", "KITE", "ARC", "ZETA"]


def font(name, size):
    return ImageFont.truetype(os.path.join(os.environ.get("WINDIR", "C:/Windows"), "Fonts", name), size)


def lettering(text, face, max_w):
    """The text in white on black, CAP high, squeezed to max_w if wider."""
    f = font(face, 200)
    box = f.getbbox(text)
    w, h = box[2] - box[0], box[3] - box[1]
    big = Image.new("L", (w + 8, h + 8), 0)
    ImageDraw.Draw(big).text((4 - box[0], 4 - box[1]), text, font=f, fill=255)
    scale = CAP / (h + 8)
    tw, th = max(1, round((w + 8) * scale)), CAP
    tw = min(tw, max_w)
    return big.resize((tw, th), Image.LANCZOS)


def main():
    atlas = Image.new("L", (W, ROW * ROWS), 0)
    names = LONG + SHORT
    for i, text in enumerate(names):
        img = lettering(text, FONTS[i % len(FONTS)], LONG_MAX if i < len(LONG) else SHORT_MAX)
        atlas.paste(img, ((W - img.width) // 2, i * ROW + (ROW - img.height) // 2))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    atlas.convert("RGB").save(OUT)
    print("wrote " + OUT)


if __name__ == "__main__":
    main()
