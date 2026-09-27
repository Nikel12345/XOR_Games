#!/usr/bin/env python3
"""Repack the radiotower_shading maps into the engine's atlas format.

Canon (TextureData.h): ORM G = LINEAR roughness, normal A = HEIGHT, a MISSING
map is white (getSurface multiplies map × factor). Scene entries: conv = AsIs.

All maps are written Y-FLIPPED (см. pack_telecom.py): меши собраны в обратной
V-конвенции, а движок сэмплит UV как есть. Зеркалить тангент-space нормаль строго
говоря надо с инверсией G — набор на диске собран без неё.

Sources are already 2048 = the atlas size, nothing is resized. The albedo is
COPIED as the original .jpg: re-encoding it to PNG would only preserve the JPEG
artifacts at six times the size.

Missing in the source: AO (-> R = 255), height (-> normal A = 255, POM flat),
emissive (-> material takes the engine's default_emissive).
"""

import numpy as np
from PIL import Image

from pack_texture import pack

SRC = "radiotower_shading_{}"
OUT = "../../game/textures/assets/telecom/radiotower_{}"


def main():
    Image.open(SRC.format("albedo.jpg")).transpose(Image.FLIP_TOP_BOTTOM).save(
        OUT.format("albedo.jpg"), quality=95, subsampling=0)
    print(f"Wrote {OUT.format('albedo.jpg')}  (Y-flipped)")

    n = np.asarray(Image.open(SRC.format("normal.png")).convert("RGB"), np.uint8)[::-1]
    a = np.full(n.shape[:2], 255, np.uint8)
    Image.fromarray(np.dstack([n, a]), "RGBA").save(OUT.format("normal_h.png"))
    print(f"Wrote {OUT.format('normal_h.png')}  (RGB copied + flat height)")

    pack(
        (2048, 2048),
        r=None,                         # нет AO → белый
        g=SRC.format("roughness.jpg"),  # уже linear roughness
        b=SRC.format("metallic.jpg"),
        a=None,
        out=OUT.format("orm.png"),
        flip_y=True,
    )


if __name__ == "__main__":
    main()
