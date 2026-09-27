#!/usr/bin/env python3
"""
Texture Channel Packer — merges separate grayscale maps into one RGBA texture.

Each source map is written into a single channel of the output image. Classic
use case is an ORM/mask atlas: e.g. Occlusion in R, Roughness in G, Metallic in
B, and something extra (height, emissive mask, ...) in A.

Notes / gotchas:
  * These are DATA maps, not color — keep them linear. Make sure the engine
    samples the packed atlas as UNORM, not SRGB.
  * Channels are treated as raw values (no gamma). Sources are converted to 8-bit
    grayscale ("L") and resized to the target size with LANCZOS.
  * Missing channels fall back to a constant (see the defaults below); pass
    --invert-<ch> to flip a channel (e.g. roughness -> glossiness).

Usage (edit the paths in main() and run, just like the example):
  python pack_texture.py

Install:
  pip install pillow numpy
"""

import numpy as np
from PIL import Image


# Per-channel default fill when a source path is None (0-255).
# Канон движка: отсутствующая карта — БЕЛАЯ, тогда getSurface отдаёт чистый фактор материала
# (ao=1, roughness/metallic = факторы). Ноль в B означал бы «металла нет никогда», и фактор
# metallic на материале переставал бы работать вовсе.
DEFAULTS = {"r": 255, "g": 255, "b": 255, "a": 255}


def load_channel(path, size, default, invert=False):
    """Load one source map as an 8-bit grayscale plane of the given size."""
    if path is None:
        plane = np.full(size[::-1], default, np.uint8)
    else:
        img = Image.open(path).convert("L").resize(size, Image.LANCZOS)
        plane = np.asarray(img, np.uint8)
    if invert:
        plane = 255 - plane
    return plane


def pack(size, r=None, g=None, b=None, a=None, out="packed.png",
         invert_r=False, invert_g=False, invert_b=False, invert_a=False,
         flip_y=False):
    """Pack up to four SEPARATE grayscale maps into one RGBA image.

    Use this for masks/data atlases (ORM etc.), where each source file is a
    single-channel map. Do NOT feed a color image (like a normal map) into one
    of these slots — it will be flattened to grayscale and its channels lost.
    For "keep an RGB map, add a grayscale into alpha" use pack_rgb_alpha().

    flip_y mirrors the result vertically — for assets whose UVs use the opposite
    V convention. Harmless for scalar maps like these; a tangent-space normal map
    mirrored this way also needs its Y (green) negated.
    """
    planes = [
        load_channel(r, size, DEFAULTS["r"], invert_r),
        load_channel(g, size, DEFAULTS["g"], invert_g),
        load_channel(b, size, DEFAULTS["b"], invert_b),
        load_channel(a, size, DEFAULTS["a"], invert_a),
    ]
    arr = np.stack(planes, axis=-1)
    if flip_y:
        arr = arr[::-1]
    Image.fromarray(arr, "RGBA").save(out)
    print(f"Wrote {out}  ({size[0]}x{size[1]}, RGBA{', Y-flipped' if flip_y else ''})")


def pack_rgb_alpha(rgb_path, a_path=None, out="packed.png",
                   size=None, a_default=255, invert_a=False, flip_y=False):
    """Keep an existing RGB image intact and put a grayscale map into alpha.

    This is what you want for e.g. a normal map (RGB = XYZ) plus a height map
    packed into the alpha channel. The RGB channels are copied verbatim — no
    grayscale flattening.

    size: (w, h) to resize everything to. If None, the RGB image's own size is
          used and the alpha map is resized to match.
    """
    rgb = Image.open(rgb_path).convert("RGB")
    if size is not None:
        rgb = rgb.resize(size, Image.LANCZOS)
    size = rgb.size

    alpha = load_channel(a_path, size, a_default, invert_a)
    arr = np.dstack([np.asarray(rgb, np.uint8), alpha])
    if flip_y:
        arr = arr[::-1]
    Image.fromarray(arr, "RGBA").save(out)
    print(f"Wrote {out}  ({size[0]}x{size[1]}, RGB copied + alpha{', Y-flipped' if flip_y else ''})")


def main():
    # Normal (RGB kept as-is) + height packed into alpha.
    pack_rgb_alpha(
        rgb_path="wood_normal.png",
        a_path="gray.png",
        out="brick_normal_h_t.png",
    )

    # --- ORM-style example: four separate grayscale maps into RGBA ---
    # pack(
    #     (2048, 2048),
    #     r="ao.png",         # Occlusion
    #     g="roughness.png",  # Roughness
    #     b="metallic.png",   # Metallic
    #     a="height.png",     # extra grayscale
    #     out="T_Material_ORMH.png",
    # )


if __name__ == "__main__":
    main()
