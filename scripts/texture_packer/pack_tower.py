#!/usr/bin/env python3
"""Repack the Building-02 source maps into the engine's atlas format.

Engine canon (see TextureData.h): ORM G = LINEAR roughness, normal A = HEIGHT
(bright = higher). Everything is normalized here, so the scene entries import
with conv = AsIs.

Sizes match the atlases they go into (Game.cpp): albedo/normal/orm 2048,
emissive 1024 — a bigger source is only downscaled by the loader anyway.

All maps are written Y-FLIPPED (см. pack_telecom.py): меши собраны в обратной
V-конвенции, а движок сэмплит UV как есть.

The source set has no height map, so the normal's alpha is filled with 255
(height = max => POM depth 1-A = 0 => flat). Repack when a height map shows up.
"""

import numpy as np
from PIL import Image

from pack_texture import pack, pack_rgb_alpha

SRC = "Building-02_{}.png"
OUT = "../../game/textures/assets/tower/tower_{}.png"
SIZE = 2048
EMISSIVE_SIZE = 1024


def resize_rgb(src, size, out):
    img = Image.open(src).convert("RGB").resize((size, size), Image.LANCZOS)
    img = img.transpose(Image.FLIP_TOP_BOTTOM)
    img.save(out)
    print(f"Wrote {out}  ({size}x{size}, RGB)")


def main():
    resize_rgb(SRC.format("albedo"), SIZE, OUT.format("albedo"))

    pack_rgb_alpha(
        rgb_path=SRC.format("normal"),
        a_path=None,              # нет карты высот в исходниках — плоская
        size=(SIZE, SIZE),
        out=OUT.format("normal_h"),
        flip_y=True,
    )

    pack(
        (SIZE, SIZE),
        r=SRC.format("ao"),
        g=SRC.format("gloss"), invert_g=True,   # gloss -> linear roughness
        b=SRC.format("metalness"),
        a=None,
        out=OUT.format("orm"),
        flip_y=True,
    )

    resize_rgb(SRC.format("emission"), EMISSIVE_SIZE, OUT.format("emissive"))


if __name__ == "__main__":
    main()
