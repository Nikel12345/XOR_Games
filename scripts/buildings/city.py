# -*- coding: utf-8 -*-
"""Сцена-город: сетка домов CITY_X x CITY_Y из набора моделей generate.py.

Запуск без параметров, файл перезаписывается. Набор моделей берётся из generate.py (MODEL_NAME/
MODEL_COUNT) — единственный источник правды о том, сколько вариантов существует.

Сцена собирается ЦЕЛИКОМ здесь, без чтения чужого scene.sheaf: свет и скайбокс — литералы ниже.
Иначе скрипт нельзя запустить дважды — первый прогон подменяет базовую сцену городом, и второй
затягивает её пулы обратно в себя, задваивая списки.
"""

import io
import json
import math
import re
import struct
import random
import sys
import collections
from bisect import bisect_right
from pathlib import Path

# Писатель формата лежит в движке (src/sheaf/sheaf.py), а движок — каталог над games, как
# XOR_ENGINE_DIR в games/CMakeLists.txt.
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "src" / "sheaf"))
import sheaf  # noqa: E402
from generate import MODEL_NAME, MODEL_COUNT, GRID, CELL, OUT_DIR, SUBMESH_SLOTS

CITY_X = 100                 # домов по X
CITY_Y = 20                 # домов по Z (вторая ось СЕТКИ — это Z сцены, не Y)

# Шаг сетки: след здания = GRID x CELL, остальное — ширина улицы. Меньше следа ставить нельзя —
# дома вложатся друг в друга.
STREET = 15.0
SPACING = GRID * CELL + STREET

# Проспект — ЛИНИЯ СЕТКИ, на которой домов нет. Шаг сетки он не меняет: ряд просто остаётся
# пустым, и разрыв выходит STREET + след + STREET = 60 юнитов вместо 15.
# Домов при этом ровно CITY_X * CITY_Y — линия ДОБАВЛЯЕТСЯ к сетке, а не забирается у неё,
# поэтому город растёт в габаритах, а не редеет.
# AVENUE_X — сколько проспектов режет ось X (сами линии тянутся вдоль Z), AVENUE_Y — наоборот.
AVENUE_X = 1
AVENUE_Y = 1

# Рабочая папка игры и папка КОНКРЕТНОЙ сцены внутри её корня сцен: движок грузит
# saved_scene/<имя сцены>, а не сам saved_scene (см. Engine::LoadScene).
GAME_DIR = Path(__file__).resolve().parents[2] / "game"
SCENE_DIR = GAME_DIR / "saved_scene" / "scene1"
SCENE_NAME = "scene.sheaf"

# По материалу на сабмеш, порядок соответствует НОМЕРАМ сабмешей (wall, podium, tech, windows):
# движок берёт материал по номеру, а не по позиции в списке. Список ОБЯЗАН покрывать все слоты
# SUBMESH_SLOTS генератора: сабмеш с номером за концом списка уходит в "material_index out of
# range" и не рисуется. Лишний материал у здания без вышки, наоборот, безвреден. Материалы свои, не общие со сценой:
# у стены и цоколя программы с frac(uv) (LitTiled / LitTransparentTiled), и меш под них размечен
# в единицах тайла — общий материал утащил бы этот тайлинг на чужую геометрию.
BUILDING_MATERIALS = ["building", "building_glass", "building_tech", "windows",
                      "tower", "telecom", "radiotower"]

# Без направленного света город чёрный, без фона — на пустом свопчейне. Значения — из штатной
# сцены game/saved_scene/scene1; ресурсы (материал/модель) движок не создаёт, они лежат в её манифестах.
SKYBOX_MATERIAL = "_skybox"
SKYBOX_MODEL = "skybox_cube"
LIGHT = collections.OrderedDict([
    ("dir_x", 0.0), ("dir_y", -1.0), ("dir_z", -0.7),
    ("r", 1.0), ("g", 1.0), ("b", 1.0), ("power", 2.5),
    ("center_x", -0.5), ("center_y", 0.0), ("center_z", 0.0),
    ("half_extent", 1.0), ("half_depth", 1.0),
    ("cascade_count", 4), ("cascade_ratio", 3.15),
])

MAT4_KEYS = ("x", "y", "z", "w", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l")
# Дефолт Transform — единичная матрица, как у PositionProxy16 в BaseComponents.h.
MAT4_DEFAULT = (1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0)


# Уровень 1 дома — та же модель без деталей. Порог — экранный РАДИУС дома (не детали), px.
DETAIL_HIDE_PX = 128.0

# Детали, которых нет на уровне 1. Окна убираются вдали потому, что они НАКЛАДКА на сплошной фасад,
# а не вырез: убрать их можно без дырок, а платит за них кадр дважды — квад далёкого окна
# субпиксельный (растеризатор всё равно выдаёт квад 2x2, три лейна из четырёх в мусор) и вдобавок
# шейдит стену вторым слоем. Номер части = номер слота generate.py (mat_ids ниже).
LEVEL1_HIDDEN = {SUBMESH_SLOTS["windows"], SUBMESH_SLOTS["tower"], SUBMESH_SLOTS["radiotower"]}



def submesh_count(model):
    """Сколько сабмешей в .bin модели (первый uint32 вершинного файла).

    Список материалов сущности обязан быть РОВНО такой длины: BatchBuilder.cpp сверяет их и на
    расхождение пишет строку в лог. Генератор пишет все слоты всегда, поэтому число одно на весь
    набор — но читается оно всё равно ИЗ ФАЙЛА: сцена обязана отвечать моделям, которые лежат на
    диске, а не тому, что о них думает импортированный generate.py."""
    with io.open(str(OUT_DIR / (model + "_v.bin")), "rb") as f:
        return struct.unpack("I", f.read(4))[0]


def pick_model(counts, taken, rng):
    """Номер модели для клетки: из тех, что не заняты соседями, самая редкая в городе.

    Раскладка ОБЯЗАНА быть непериодической. Прежняя формула model = (ix + step*iz) % m давала
    ровно те же два свойства (поровну домов на модель, разные соседи), но одинаковые модели
    ложились на параллельные диагонали — а модели различаются высотой в три раза, и город
    читался диагональными полосами вместо сетки.

    Жадность даёт и то, и другое без периода: сосед исключён по списку, а выбор самой редкой
    из оставшихся держит счётчики в пределах единицы друг от друга. Ничья решается броском —
    он и убирает регулярность."""
    pool = [m for m in range(MODEL_COUNT) if m not in taken] or list(range(MODEL_COUNT))
    rarest = min(counts[m] for m in pool)
    return rng.choice([m for m in pool if counts[m] == rarest])


def avenue_gaps(rows, count, rng):
    """Номера ПРОМЕЖУТКОВ между рядами домов, отданные под проспекты.

    Проспект ставится в промежуток между двумя соседними рядами (их rows - 1), и не больше
    одного в промежуток. Этим обе частотные оговорки выполняются сами собой: два проспекта
    подряд невозможны (между ними всегда остаётся ряд домов), и к краю карты проспект не
    прижмётся — крайних промежутков в списке просто нет. Тот же приём, что у техпояса в
    generate.py, только минимальный зазор здесь — один ряд домов."""
    assert count <= rows - 1, "%d avenues need at least %d building rows" % (count, count + 1)
    return sorted(rng.sample(range(1, rows), count))


def city_grid(av_x, av_z, rng):
    """-> (ix, iz, номер модели 1..MODEL_COUNT, четверть поворота, tx, tz).

    ix/iz — номера в сетке ДОМОВ, а позиция берётся из сетки ЛИНИЙ: ряд съезжает на столько
    шагов, сколько проспектов прошло до него."""
    ox = (CITY_X + len(av_x) - 1) * 0.5
    oz = (CITY_Y + len(av_z) - 1) * 0.5
    counts = collections.Counter()
    chosen = {}
    for iz in range(CITY_Y):
        lz = iz + bisect_right(av_z, iz)
        for ix in range(CITY_X):
            lx = ix + bisect_right(av_x, ix)
            # Соседи, которые УЖЕ расставлены: сторона слева, ряд снизу и обе диагонали.
            # Те, что дальше по обходу, проверят себя об эту клетку сами.
            taken = {chosen.get(c) for c in ((ix - 1, iz), (ix, iz - 1),
                                             (ix - 1, iz - 1), (ix + 1, iz - 1))}
            model = pick_model(counts, taken, rng)
            counts[model] += 1
            chosen[(ix, iz)] = model
            # Разворот кратен 90°: след здания квадратный, поэтому линия улиц не ломается, а
            # повторяемость набора моделей на тысяче домов на глаз пропадает. Бросок, а не
            # формула, по той же причине, что и у выбора модели.
            quarter = rng.randrange(4)
            yield ix, iz, model + 1, quarter, (lx - ox) * SPACING, (lz - oz) * SPACING


def add_renderable(t, models, parts, hidden=None):
    """Renderable в раскладке движка (MakeSaveRenderable в Engine.cpp). parts[i] — материалы частей
    объекта i; hidden — номера частей, которых нет на уровне 1 модели; без него уровень один."""
    count = len(models)
    c = t.component("Renderable")
    c.field("visible", sheaf.BOOL, [True] * count, default=True)
    c.field("alpha", sheaf.F32, [1.0] * count, default=1.0)
    c.field("flags", sheaf.U32, [0] * count)
    c.field("model", sheaf.STR, models)
    c.field("mat_lod0", sheaf.STR, parts, flags=sheaf.LIST | sheaf.NULLABLE)
    if hidden is not None:
        c.field("mat_lod1", sheaf.STR, [[None if k in hidden else m for k, m in enumerate(row)] for row in parts],
                flags=sheaf.LIST | sheaf.NULLABLE)


def build_scene(rng):
    models = [MODEL_NAME + str(n + 1) for n in range(MODEL_COUNT)]
    # Номера материалов — префикс списка: сабмеш адресует материал по НОМЕРУ, поэтому
    # обрезать можно только хвост, и ровно до числа сабмешей модели.
    mat_ids = {n: list(range(submesh_count(MODEL_NAME + str(n))))
               for n in range(1, MODEL_COUNT + 1)}
    over = [n for n, ids in mat_ids.items() if len(ids) > len(BUILDING_MATERIALS)]
    assert not over, "models %s have more submeshes than BUILDING_MATERIALS" % over

    av_x = avenue_gaps(CITY_X, AVENUE_X, rng)
    av_z = avenue_gaps(CITY_Y, AVENUE_Y, rng)
    cells = list(city_grid(av_x, av_z, rng))
    cols = collections.OrderedDict((k, []) for k in MAT4_KEYS)
    parts, model_col = [], []
    for _, _, model, quarter, tx, tz in cells:
        model_col.append(models[model - 1])
        parts.append([BUILDING_MATERIALS[i] for i in mat_ids[model]])
        a = 0.5 * math.pi * quarter
        ca, sa = float(round(math.cos(a))), float(round(math.sin(a)))
        # Матрица row-major, перенос — в четвёртом СТОЛБЦЕ (колонки w/d/h), как её пишет
        # ObjectManager::SaveScene.
        row = [ca, 0.0, sa, tx,
               0.0, 1.0, 0.0, 0.0,
               -sa, 0.0, ca, tz,
               0.0, 0.0, 0.0, 1.0]
        for key, v in zip(MAT4_KEYS, row):
            cols[key].append(v)

    # Таблицы — по ключу архетипа (имена компонентов по алфавиту через запятую), как их пишет
    # SaveScene движка: тогда пересохранение из редактора не переставляет таблицы.
    w = sheaf.Writer()
    sun = w.table(1)
    light = sun.component("DirectLight")
    for k, v in LIGHT.items():
        light.field(k, sheaf.U32 if isinstance(v, int) else sheaf.F32, [v])
    sun.component("ShadowCaster")
    add_renderable(w.table(1), [SKYBOX_MODEL], [[SKYBOX_MATERIAL]])
    town = w.table(len(cells))
    add_renderable(town, model_col, parts, LEVEL1_HIDDEN)
    tr = town.component("Transform")
    for key, default in zip(MAT4_KEYS, MAT4_DEFAULT):
        tr.field(key, sheaf.F32, cols[key], default=default)
    return w.finish(), cells, av_x, av_z


def sync_models():
    """Приводит записи зданий в models.json к тому, что реально лежит на диске.

    Реестр ведётся ЗДЕСЬ, а не руками: набор моделей задаёт генератор (MODEL_COUNT), а
    незарегистрированную модель движок не находит и молча пропускает сущность. Дыры при этом
    ложатся не как попало, а по диагоналям — номер модели идёт латинским квадратом, — и город
    выходит в полоску вместо сетки. Предупреждения в логе тут мало: его легко проглядеть,
    а сцена уже собрана.

    Чужие записи (машины, скайбокс, вышки) не трогаются вовсе — только имена MODEL_NAME+цифра,
    и только те, чьи файлы есть на диске. Пути ОТНОСИТЕЛЬНЫЕ от корня проекта: абсолютный путь
    сцена не переживёт (см. правило путей в манифестах).
    -> строки отчёта."""
    path = SCENE_DIR / "models.json"
    try:
        doc = json.load(io.open(str(path), encoding="utf-8"))
        models = doc["models"]
    except (IOError, ValueError, KeyError):
        return ["models.json not readable - registration NOT synced"]

    want = collections.OrderedDict()
    missing_files = []
    for n in range(1, MODEL_COUNT + 1):
        name = MODEL_NAME + str(n)
        stem = OUT_DIR / (name + "_v.bin")
        if not stem.exists():
            missing_files.append(name)
            continue
        # Пути относительны РАБОЧЕЙ ПАПКЕ игры (game).
        rel = OUT_DIR.relative_to(GAME_DIR).as_posix()
        want[name] = collections.OrderedDict([
            ("name", name),
            ("vertex", "%s/%s_v.bin" % (rel, name)),
            ("index", "%s/%s_i.bin" % (rel, name)),
            ("anchor", 0),
            ("pool", "PosUVNorm"),
            ("lods", [collections.OrderedDict([
                ("vertex", "%s/%s_v.bin" % (rel, name)),
                ("index", "%s/%s_i.bin" % (rel, name)),
                ("switch", DETAIL_HIDE_PX),
            ])]),
        ])

    generated = re.compile(r"^%s\d+$" % re.escape(MODEL_NAME))
    kept = [m for m in models if not generated.match(m.get("name", ""))]
    had = {m["name"] for m in models if generated.match(m.get("name", ""))}
    doc["models"] = kept + list(want.values())
    io.open(str(path), "w", encoding="utf-8", newline="\n").write(
        json.dumps(doc, indent=4, ensure_ascii=False) + "\n")

    added = [n for n in want if n not in had]
    dropped = sorted(had - set(want))
    out = ["models.json: %d building entries (added %d, dropped %d)"
           % (len(want), len(added), len(dropped))]
    if dropped:
        out.append("dropped stale: " + ", ".join(dropped))
    if missing_files:
        out.append("NO .bin on disk (run generate.py first): " + ", ".join(missing_files))
    return out


def main():
    rng = random.Random()
    registry = sync_models()
    data, cells, av_x, av_z = build_scene(rng)
    out = SCENE_DIR / SCENE_NAME
    out.write_bytes(data)

    per = collections.Counter(c[2] for c in cells)
    grid = {(c[0], c[1]): c[2] for c in cells}
    same = sum(1 for (ix, iz), m in grid.items()
               for d in ((1, 0), (0, 1), (1, 1), (1, -1))
               if grid.get((ix + d[0], iz + d[1])) == m)
    span_x = (CITY_X + len(av_x) - 1) * SPACING + GRID * CELL
    span_z = (CITY_Y + len(av_z) - 1) * SPACING + GRID * CELL
    # Промежуток k становится линией k + (сколько проспектов встало до него).
    line_x = [k + i for i, k in enumerate(av_x)]
    line_z = [k + i for i, k in enumerate(av_z)]

    print("city %dx%d = %d buildings from %d models" % (CITY_X, CITY_Y, len(cells), MODEL_COUNT))
    print("  spacing %.0f (footprint %.0f + street %.0f), extent %.0f x %.0f units"
          % (SPACING, GRID * CELL, STREET, span_x, span_z))
    print("  per model: %s" % " ".join("%d:%d" % kv for kv in sorted(per.items())))
    print("  adjacent same-model pairs: %d" % same)
    print("  avenues %.0f units wide: %d across X at lines %s, %d across Z at lines %s"
          % (2.0 * SPACING - GRID * CELL, len(line_x), line_x, len(line_z), line_z))
    print("  materials per building: %s" % ", ".join(BUILDING_MATERIALS))
    print("  submeshes per model: %s" % " ".join(
        "%d:%d" % (n, submesh_count(MODEL_NAME + str(n))) for n in range(1, MODEL_COUNT + 1)))
    for line in registry:
        print("  %s" % line)
    print("  -> %s" % out)


if __name__ == "__main__":
    main()
