#!/usr/bin/env python3
"""Repack the SignBoard maps into the engine's atlas format.

Two materials in the FBX, so two complete sets: the sign faces ("board") and the
frames/brackets ("metal"). Submesh order in the models matches: 0 = board, 1 = metal.

Наблюдения по исходникам, которые определили раскладку:
  * signboard_mat02 — НЕ карта свойств, а МАСКА выреза: 73% белого, 26% чёрного,
    между ними 0.8% (jpeg-кайма). Под белым лежит рисунок вывески (яркость 140,
    насыщенность 116), под чёрным — пустой фон (яркость 5). Едет в АЛЬФУ альбедо,
    где её читает alpha-test в main_pass/surface.hlsl.
  * metal_rough03 — вопреки имени, roughness НЕ для металла, а для атласа вывесок:
    корреляция с яркостью signboard_dif02 = 0.966 (та же раскладка тайлов).
    Металлу принадлежит metal_rough (4096, тёмный, полосатый).

Разрешения: board оставлен НАТИВНЫМ 2000 — он и так занимает слой атласа целиком
(2000 + 2×16 гаттера ≤ 2048), поэтому ресемплить его не за чем, а текст на вывесках
от этого только выигрывает. Металл ужат до 1024: тайловый серый без деталей.

Canon (TextureData.h): ORM G = LINEAR roughness, normal A = HEIGHT, отсутствующая
карта = белая (getSurface умножает карту на фактор). Карт высот и AO в наборе нет.
Всё пишется Y-FLIPPED — как остальные наборы, см. pack_telecom.py.
"""

import numpy as np
from PIL import Image

from pack_texture import pack, pack_rgb_alpha

SRC = "raw/{}"
OUT = "../../game/textures/assets/signs/signs_{}"

BOARD = 2000
METAL = 1024


def flip_rgb(src, size, out):
    img = Image.open(src).convert("RGB")
    if img.size != (size, size):
        img = img.resize((size, size), Image.LANCZOS)
    img.transpose(Image.FLIP_TOP_BOTTOM).save(out)
    print(f"Wrote {out}  ({size}x{size}, RGB, Y-flipped)")


def normal_h(src, size, out):
    """RGB нормали + плоская высота в альфе (карты высот в наборе нет)."""
    img = Image.open(src).convert("RGB")
    if img.size != (size, size):
        img = img.resize((size, size), Image.LANCZOS)
    n = np.asarray(img, np.uint8)[::-1]
    a = np.full(n.shape[:2], 255, np.uint8)
    Image.fromarray(np.dstack([n, a]), "RGBA").save(out)
    print(f"Wrote {out}  ({size}x{size}, RGB copied + flat height, Y-flipped)")


def main():
    # ── Вывески ───────────────────────────────────────────────────────────────
    # Альбедо + маска выреза в альфе. RGB не ресемплится (размер совпадает),
    # маска доводится до того же размера тем же LANCZOS — мягкая кайма ложится
    # ровно на порог 0.5 alpha-теста.
    pack_rgb_alpha(
        rgb_path=SRC.format("signboard_dif02.jpg"),
        a_path=SRC.format("signboard_mat02.jpg"),
        size=(BOARD, BOARD),
        out=OUT.format("board_albedo.png"),
        flip_y=True,
    )
    normal_h(SRC.format("signboard_nor02.png"), BOARD, OUT.format("board_normal_h.png"))
    pack(
        (BOARD, BOARD),
        r=None,                              # нет AO → белый
        g=SRC.format("metal_rough03.jpg"),   # roughness ВЫВЕСОК (см. шапку)
        b=None,                              # карты металла нет → белый, металл решает фактор
        a=None,
        out=OUT.format("board_orm.png"),
        flip_y=True,
    )

    # ── Металл рам и кронштейнов ──────────────────────────────────────────────
    flip_rgb(SRC.format("metal_dif.jpg"), METAL, OUT.format("metal_albedo.png"))
    normal_h(SRC.format("metal_nor.jpg"), METAL, OUT.format("metal_normal_h.png"))
    pack(
        (METAL, METAL),
        r=None,
        g=SRC.format("metal_rough.jpg"),
        b=None,
        a=None,
        out=OUT.format("metal_orm.png"),
        flip_y=True,
    )


if __name__ == "__main__":
    main()
