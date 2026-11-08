#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

"""Banding in the smooth gradients (skies, clouds) of screenshots.

The image is cut into 32x32 blocks; a block counts as a smooth gradient when its luma spans
2..24 levels and changes by at most 4 levels between neighbouring pixels. Over those blocks:

- levels: distinct RGB colours per block (more is a finer gradient),
- run: mean length of runs of identical pixels along rows (longer is wider bands),
- steps: histogram of the absolute luma step between horizontal neighbours (0, 1, 2, 3, 4):
  a stream quantized to 8 bits shows 0 and 1; a range expansion (limited to full, x1.164)
  turns some 1-steps into 2-steps.

usage: banding.py <png or directory> [...]   (needs numpy and Pillow)
"""

import sys
from pathlib import Path

import numpy as np
from PIL import Image

BLOCK = 32


def analyse(path):
    rgb = np.asarray(Image.open(path).convert("RGB"), dtype=np.int32)
    luma = (rgb[..., 0] * 2126 + rgb[..., 1] * 7152 + rgb[..., 2] * 722 + 5000) // 10000
    h, w = luma.shape
    blocks = levels = runs = run_count = 0
    steps = np.zeros(5, dtype=np.int64)
    for y in range(0, h - BLOCK + 1, BLOCK):
        for x in range(0, w - BLOCK + 1, BLOCK):
            block = luma[y:y + BLOCK, x:x + BLOCK]
            span = int(block.max() - block.min())
            if span < 2 or span > 24:
                continue
            diff = np.abs(np.diff(block, axis=1))
            if diff.max() > 4 or np.abs(np.diff(block, axis=0)).max() > 4:
                continue
            blocks += 1
            colours = rgb[y:y + BLOCK, x:x + BLOCK].reshape(-1, 3)
            levels += len(np.unique(colours[:, 0] * 65536 + colours[:, 1] * 256 + colours[:, 2]))
            steps += np.bincount(diff.ravel(), minlength=5)[:5]
            same = np.all(np.diff(rgb[y:y + BLOCK, x:x + BLOCK], axis=1) == 0, axis=2)
            changes = BLOCK - 1 - same.sum(axis=1)
            runs += BLOCK * BLOCK
            run_count += int((changes + 1).sum())
    if blocks == 0:
        return f"{path}: no smooth gradient blocks"
    total = steps.sum()
    hist = " ".join(f"{i}:{100.0 * steps[i] / total:.1f}%" for i in range(5))
    return (f"{path}: {blocks} gradient blocks, {levels / blocks:.1f} colours/block, "
            f"run {runs / run_count:.2f} px, steps {hist}")


def main():
    paths = []
    for arg in sys.argv[1:]:
        p = Path(arg)
        paths += sorted(p.glob("*.png")) if p.is_dir() else [p]
    for path in paths:
        print(analyse(path))


if __name__ == "__main__":
    main()
