#!/usr/bin/env python3
"""Repack the RadioTowerTrim source maps into the engine's atlas format.

Engine canon (TextureData.h): ORM G = LINEAR roughness, normal A = HEIGHT
(bright = higher), a MISSING map is white (getSurface multiplies the map by the
factor, so 255 = "factor passes through untouched"). Everything is normalized
here, so the scene entries import with conv = AsIs.

All maps are written Y-FLIPPED: the meshes were authored with the opposite V
convention, and the engine samples mesh UVs raw (no flip anywhere in the shader
or the loader). NB: a vertical mirror of a TANGENT-SPACE normal map should also
negate Y (G = 255 - G) — not done here, see the note next to the normal below.

The set is a trim sheet: all six towers share it. Source is already 2048 = the
atlas size (Game.cpp), so nothing is resized — the albedo alpha is a BINARY
cutout mask and any resampling would smear it into gray.

Missing in the source: AO (-> R = 255), height (-> normal A = 255, POM flat),
emissive (no file at all — the material references the engine's default_emissive).
The metallic map is present but constant 0.
"""

import numpy as np
from PIL import Image

from pack_texture import pack

SRC = "PropsTrim_RadioTowerTrim_{}.png"
OUT = "../../game/textures/assets/telecom/telecom_{}.png"


def main():
    # Albedo: RGB + бинарная маска выреза в альфе (решётки/лестницы) — сохраняем как есть.
    # convert("RGBA") ДО флипа: редактор, в котором картинку переворачивали руками, альфу теряет.
    alb = Image.open(SRC.format("BaseColor")).convert("RGBA").transpose(Image.FLIP_TOP_BOTTOM)
    alb.save(OUT.format("albedo"))
    print(f"Wrote {OUT.format('albedo')}  (RGBA, cutout mask kept, Y-flipped)")

    # Normal: RGB источника + плоская высота (карты высот в наборе нет).
    # Зеркалим только пиксели: G (= Y тангент-пространства) при этом остаётся от исходной
    # ориентации, то есть рельеф освещается «наизнанку» по вертикали. Инверсия G здесь НЕ
    # делается сознательно — набор на диске собран так же, и правку надо вносить целиком.
    n = np.asarray(Image.open(SRC.format("Normal")).convert("RGB"), np.uint8)[::-1]
    a = np.full(n.shape[:2], 255, np.uint8)
    Image.fromarray(np.dstack([n, a]), "RGBA").save(OUT.format("normal_h"))
    print(f"Wrote {OUT.format('normal_h')}  (RGB copied + flat height, Y-flipped)")

    pack(
        (2048, 2048),
        r=None,                                       # нет AO → белый
        g=SRC.format("Roughness@channels=G"),         # уже linear roughness, инверсия не нужна
        b=SRC.format("Metallic"),
        a=None,
        out=OUT.format("orm"),
        flip_y=True,
    )


if __name__ == "__main__":
    main()
