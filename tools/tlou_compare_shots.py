#!/usr/bin/env python3
"""Compares the frames two replays saved at the same moments (tools/tlou_replay.py --shots).

Pairs the frames by shot index, prints how much each pair differs and writes, per pair, an image
with the first run, the second and their difference side by side. Needs numpy and Pillow.

  python tools/tlou_compare_shots.py <captures A> <captures B> <out folder>

The replays drift apart after loads (a load can take seconds longer), so compare pairs from the
same scene; the differences of a pair right after a load are mostly timing.
"""
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def frames(folder):
    out = []
    for raw in sorted(Path(folder).glob("output*.raw")):
        size = re.search(r"_(\d+)x(\d+)\.raw$", raw.name)
        if not size:
            continue
        w, h = int(size.group(1)), int(size.group(2))
        data = raw.read_bytes()
        if len(data) < w * h * 4:
            continue
        image = Image.frombytes("RGBA", (w, h), data[:w * h * 4]).convert("RGB")
        image.thumbnail((960, 540))
        out.append(image)
    return out


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    a, b, out = frames(sys.argv[1]), frames(sys.argv[2]), Path(sys.argv[3])
    shots_file = Path(sys.argv[1]) / "shots.txt"
    shots = shots_file.read_text().split() if shots_file.exists() else []
    out.mkdir(parents=True, exist_ok=True)
    print(f"{len(a)} frames in A, {len(b)} in B")
    for i in range(min(len(a), len(b))):
        x = np.asarray(a[i], dtype=np.int16)
        y = np.asarray(b[i].resize(a[i].size), dtype=np.int16)
        d = np.abs(x - y).sum(axis=2)
        side = Image.new("RGB", (a[i].width * 3, a[i].height))
        side.paste(a[i], (0, 0))
        side.paste(b[i].resize(a[i].size), (a[i].width, 0))
        side.paste(Image.fromarray(np.clip(d * 2, 0, 255).astype(np.uint8)).convert("RGB"),
                   (a[i].width * 2, 0))
        side.save(out / f"pair_{i:02d}.png")
        t = shots[i] if i < len(shots) else "?"
        print(f"shot {i:2d} (t={t:>5} s): mean difference {float(d.mean()):6.1f}, "
              f"{float((d > 60).mean() * 100):5.1f}% of the pixels")
    return 0


if __name__ == "__main__":
    sys.exit(main())
