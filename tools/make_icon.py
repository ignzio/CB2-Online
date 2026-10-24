"""Generates assets/icon.ico: a wooden crate with a "2", drawn from scratch (no game artwork). Needs Pillow."""
import os
from PIL import Image, ImageDraw, ImageFont

S = 512
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

wood, dark, light, edge = (205, 128, 48), (122, 66, 22), (236, 168, 84), (70, 36, 10)
m = 28
d.rounded_rectangle([m, m, S - m, S - m], radius=44, fill=wood, outline=edge, width=14)
# frame
f = 70
d.rounded_rectangle([m + 10, m + 10, S - m - 10, S - m - 10], radius=36, outline=dark, width=f // 2)
d.rounded_rectangle([m + 10 + f // 2, m + 10 + f // 2, S - m - 10 - f // 2, S - m - 10 - f // 2], radius=18, fill=light, outline=edge, width=8)
# planks inside the frame
inner = [m + 10 + f // 2 + 8, m + 10 + f // 2 + 8, S - m - 10 - f // 2 - 8, S - m - 10 - f // 2 - 8]
step = (inner[3] - inner[1]) // 4
for i in range(1, 4):
    y = inner[1] + i * step
    d.line([inner[0], y, inner[2], y], fill=dark, width=6)
# the number
try:
    font = ImageFont.truetype("arialbd.ttf", 250)
except OSError:
    font = ImageFont.load_default()
txt = "2"
bbox = d.textbbox((0, 0), txt, font=font)
w, h = bbox[2] - bbox[0], bbox[3] - bbox[1]
cx, cy = S // 2 - bbox[0] - w // 2, S // 2 - bbox[1] - h // 2
for dx, dy in ((8, 8), (-4, 8), (8, -4)):
    d.text((cx + dx, cy + dy), txt, font=font, fill=edge)
d.text((cx, cy), txt, font=font, fill=(255, 250, 235))
# corner nails
for x, y in ((m + 40, m + 40), (S - m - 40, m + 40), (m + 40, S - m - 40), (S - m - 40, S - m - 40)):
    d.ellipse([x - 9, y - 9, x + 9, y + 9], fill=edge)

out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "assets")
os.makedirs(out, exist_ok=True)
img.save(os.path.join(out, "icon.ico"), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
img.resize((256, 256), Image.LANCZOS).save(os.path.join(out, "icon_preview.png"))
print("wrote", os.path.join(out, "icon.ico"))
