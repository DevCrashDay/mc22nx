import os, sys
from PIL import Image, ImageStat
frames = os.path.expandvars(r"%APPDATA%\Ryujinx\sdcard\mc2\frames")
base = r"%USERPROFILE%\Downloads\mc2\baseline\pc-gl"
out = sys.argv[1]
pairs = [("00300", "01_logo_rockstar"), ("00600", "02_logo_sandiego"), ("01800", "03_menu"), ("03600", "04_race_start"), ("04500", "05_race_ingame")]
ims = []
for n, b in pairs:
    p = os.path.join(frames, f"frame_{n}.ppm")
    if not os.path.exists(p):
        continue
    a = Image.open(p).convert("RGB")
    r = Image.open(os.path.join(base, b + ".png")).convert("RGB").resize(a.size)
    diff = sum(1 for x, y in zip(a.getdata(), r.getdata()) if max(abs(x[i] - y[i]) for i in range(3)) > 8)
    print(n, "mean", [round(v) for v in ImageStat.Stat(a).mean], f"differs {100 * diff / (a.size[0] * a.size[1]):.1f}%")
    ims += [a, r]
w, h = ims[1].size
c = Image.new("RGB", (w * 2, h * (len(ims) // 2)))
for i, im in enumerate(ims):
    c.paste(im.resize((w, h)), ((i % 2) * w, (i // 2) * h))
c.save(out)
