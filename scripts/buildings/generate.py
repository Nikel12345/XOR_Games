#!/usr/bin/env python3
"""
Генератор моделей небоскрёбов. Один запуск = одна модель (два .bin в game/models/buildings).

Сделано: форма (сетка 3x3), высота (одна на все блоки), крыша (элемент на блок).
Дальше по плану: декор -> окна.

Формат вершины — пул PosUVNorm движка (44 байта, интерлив):
  float x, y, z | float u, v | float nx, ny, nz | float tx, ty, tz
Формат файла — как у scripts/model_loader/convert_model.py:
  вершинный:  uint32 submesh_count, затем submesh_count x (5 x uint32), затем вершины;
  индексный:  сырой массив uint32.
"""

import math
import random
import struct
from collections import deque
from pathlib import Path

# ── Параметры формы ───────────────────────────────────────────────────────────
GRID = 3          # сетка GRID x GRID ячеек
CELL = 10.0       # сторона ячейки в юнитах мира
PICK_CHANCE = 0.5 # шанс, что очередной кандидат BFS попадёт в форму (иначе просто пропущен)

# ── Параметры высоты ──────────────────────────────────────────────────────────
# Высота МЕРИТСЯ В ЭТАЖАХ и только потом переводится в юниты: этаж — это шаг, к которому
# обязаны привязаться ряды окон, поэтому непрерывной высоты у блока быть не должно.
FLOOR_H = 3.5            # высота этажа в юнитах
FLOORS = (15, 60)        # этажей у здания; высота ОДНА на все блоки

# ── Вертикальные участки фасада ───────────────────────────────────────────────
# Фасад режется по высоте на СЕГМЕНТЫ, и это основной примитив, а не частный случай цоколя:
# у сегмента свой диапазон высоты, свой сабмеш, свой вынос наружу и (дальше по плану) своя
# сетка со своим набором декора. Цоколь ЗАБИРАЕТ нижние этажи у корпуса, венец — верхние,
# поэтому ряды окон живут только в диапазоне корпуса.
PODIUM_MIN_H = 5.0       # МИНИМАЛЬНАЯ высота цоколя в юнитах: вестибюль ниже неё читается
                         # не как цоколь, а как случайная ступенька. Задана в юнитах, а не в
                         # этажах, но округляется ВВЕРХ до целого числа этажей — высота здания
                         # обязана оставаться кратной шагу этажа, иначе поедет сетка окон.
PODIUM_MAX_FLOORS = 3
PODIUM_INSET = 0.4       # цоколь УЖЕ корпуса: корпус нависает над ним этим свесом
CROWN_FLOORS = (0, 2)    # этажей между верхним рядом окон и крышей
TECH_FLOORS = 1          # высота технического пояса в этажах
TECH_INSET = 0.3         # пояс уже корпуса — как цоколь, только в середине здания
TECH_MIN_GAP = 5         # этажей до другого пояса, до крыши и до цоколя
PARAPET_H = 1.4          # высота стены крыши над плоскостью крыши
PARAPET_T = 0.35         # её толщина внутрь от плоскости стены

# Размер панели, на которые режется ЛЮБАЯ поверхность стены (см. emit_face_panels): и стены,
# и полки выреза, и лента парапета. Это ГЛАВНАЯ цена модели: панель стоит 4 вершины, а число
# панелей растёт как 1/размер^2 — 85% вершин здания уходит на них, а не на его форму.
# Размер обязан отвечать тому, что НАРИСОВАНО на текстуре: один этаж на картинку -> 1,
# два этажа -> 2. Меряется в этажах, чтобы ряды панелей ложились на этажи без подгонки.
PANEL_FLOORS = 1

# ── Окна ──────────────────────────────────────────────────────────────────────
# Окно занимает ячейку сетки ЦЕЛИКОМ: рама, откос и поля стены нарисованы в самом тайле,
# поэтому "простенок между окнами" — вопрос текстуры, а не геометрии, и фасад стоит ровно
# столько же квадов, сколько стоила бы голая стена. Глубины нет: стекло вровень со стеной.
WINDOW_SHEET = (4, 4)      # раскладка листа вариантов: 16 тайлов
WINDOW_TEXTURE = "game/textures/assets/windows.jpg"
WINDOW_TILE_PAD = 0.002    # гуттер внутрь тайла в UV: без него мип затянет соседний тайл в край окна
WINDOW_TILE_ASPECT_FALLBACK = 1.49   # если текстуры нет на месте — аспект тайла windows.jpg

# Ячейка фасада: проём занимает ДОЛЮ этажа, остальное по вертикали — подоконная и надоконная
# части стены. Ширина ячейки идёт от аспекта тайла, поэтому окно не растянуто.
WINDOW_CELL_FLOORS = 0.40    # доля этажа под сам проём
WINDOW_SILL_FLOORS = 0.35    # доля этажа под подоконную часть (остаток сверху — надоконная)

# Рисунок фасада строится ТРЕМЯ слоями, и порядок принципиален:
#   1. здание СПЛОШЬ фасад;
#   2. на него кладутся ФАСАДНЫЕ КВАДЫ — прямоугольники, которые останутся стеной гарантированно.
#      Всё здание они НЕ покрывают;
#   3. на каждом этаже группы окон ХОТЯТ встать, и встают везде, куда не попал фасадный квад.
# Из-за третьего слоя ряд, задуманный как [0,1,1,1,0], может выйти [0,1,0,0,0] — и это не сбой,
# а сам механизм: он ломает регулярность, которой страдала решётка "полоса x группа".
WALL_QUAD_COLS = (1, 5)      # ширина фасадного квада в колоннах
WALL_QUAD_ROWS = (2, 8)      # его высота в этажах
WALL_QUAD_COVER = 0.45       # какую долю площади грани они пытаются занять (с перекрытиями)
WINDOW_RUN_COLS = (1, 5)     # длина группы окон в ряду
WINDOW_GAP_COLS = (1, 3)     # промежуток между группами в ряду


def jpeg_size(path):
    """Ширина/высота JPEG из маркера SOF. Аспект ячейки обязан идти ОТ ТЕКСТУРЫ, а не быть
    вписанной константой: лист не квадратный, и разъезд этих двух чисел растянет окно."""
    try:
        d = open(path, "rb").read()
    except OSError:
        return None
    i = 2
    while i + 9 < len(d):
        if d[i] != 0xFF:
            i += 1
            continue
        m = d[i + 1]
        if m in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
            return ((d[i + 7] << 8) | d[i + 8], (d[i + 5] << 8) | d[i + 6])
        i += 2 + ((d[i + 2] << 8) | d[i + 3])
    return None


def window_tile_aspect():
    px = jpeg_size(str(Path(__file__).resolve().parents[2] / WINDOW_TEXTURE))
    if not px:
        return WINDOW_TILE_ASPECT_FALLBACK
    return (px[0] / WINDOW_SHEET[0]) / (px[1] / WINDOW_SHEET[1])

# Номер сабмеша = индекс материала у сущности, поэтому таблица ФИКСИРОВАНА: слот, однажды
# получивший номер, его не меняет — иначе правка генератора молча переставила бы материалы на
# уже расставленных в сцене зданиях.
#
# Отсюда правило: новый слот ДОПИСЫВАЕТСЯ В КОНЕЦ, а не вставляется. Номер, зарезервированный
# под ещё не сделанный слот, оставляет ДЫРКУ в нумерации, а движок берёт материал по номеру
# сабмеша, а не по его порядку в файле: сущности пришлось бы держать лишний материал-заглушку,
# иначе сабмеш за дыркой уходит в "material_index out of range" и не рисуется вовсе.
SUBMESH_SLOTS = {"wall": 0, "podium": 1, "tech": 2, "windows": 3,
                 "tower": 4, "telecom": 5, "radiotower": 6}

# Слоты, чей материал сидит на программе с frac(uv) (LitTiled, game/shaders/building_surface.hlsl):
# им повтор текстуры даёт шейдер, и геометрия кладёт один квад с uv 0..N вместо N квадов с uv 0..1.
# Слот, оставшийся на движковом Lit, обязан остаться панельным — иначе его текстура растянется
# на всю поверхность вместо повтора.
TILED_SLOTS = {"wall", "podium", "tech"}

# Окна НАКЛАДЫВАЮТСЯ на сплошную стену, а не вырезаются из неё. Стена тогда — один тайлящийся
# квад на пролёт, и построчная структура (подоконные/надоконные ленты, слитые прямоугольники
# сплошных ячеек) исчезает вместе с вершинами, которых она стоила.
# Цена: окно и стена КОМПЛАНАРНЫ, поэтому окно приподнято на WINDOW_LIFT. Одного подъёма мало —
# у d32-глубины с near=0.01/far=5000 разрешение падает как z^2 (на 400 юнитах это ~2 юнита),
# так что материал окон обязан идти с depth bias; подъём лишь страхует ближний план.
WINDOW_OVERLAY = True
WINDOW_LIFT = 0.02

# ── Лёгкий силуэт (эксперимент) ───────────────────────────────────────────────
# Здание СПЛОШЬ из нескольких квадов: окон нет, панелей нет, текстура не тайлится (uv 0..1 на
# квад), а блоки ОДНОГО уровня сливаются в общий контур — сторона из четырёх ячеек уходит одним
# квадом вместо четырёх. Уровни между собой НЕ сливаются: техпояс их разделяет, и каждый остаётся
# своей группой вершин в том же сабмеше — сваривать их значило бы терять и пояс, и вынос.
# Смысл режима — замер: узнать, сколько кадра стоит геометрия здания, а не его материалы.
# Панельный режим (WINDOW_OVERLAY и всё, что вокруг фасада) при False остаётся как был.
SIMPLE_SHELL = True

SIDE_FACES = (0, 1, 4, 5)   # грани +-X и +-Z; 2/3 — крышки

VERTEX_STRUCT = "fffffffffff"   # 11 float = 44 байта
SUBMESH_ENTRY = "IIIII"         # vOffset, iOffset, vCount, iCount, material_index

OUT_DIR = Path(__file__).resolve().parents[2] / "game" / "models" / "buildings"
# Имена ФИКСИРОВАННЫЕ и перезаписываются каждым запуском: building1 .. buildingN. Цифра — номер
# варианта в наборе, а не случайный суффикс, поэтому ссылки на модели в манифестах сцены не
# протухают, сколько бы раз ни перегенерировали набор.
MODEL_NAME = "building"
MODEL_COUNT = 30

# Грани единичного куба в +-1-пространстве: c — стартовый угол, U/V — рёбра, N — внешняя нормаль.
# cross(U, V) = N, поэтому обход p0=c, p1=c+U, p2=c+U+V, p3=c+V даёт CCW наружу.
# Таблица скопирована из движкового генератора "cube" (Engine.cpp) — обмотка и развёртка обязаны
# совпадать с ней, иначе фасады окажутся вывернуты относительно остальных мешей сцены.
FACES = [
    ((1, -1, 1),  (0, 0, -2), (0, 2, 0),  (1, 0, 0)),    # +X
    ((-1, -1, -1),(0, 0, 2),  (0, 2, 0),  (-1, 0, 0)),   # -X
    ((-1, 1, 1),  (2, 0, 0),  (0, 0, -2), (0, 1, 0)),    # +Y
    ((-1, -1, -1),(2, 0, 0),  (0, 0, 2),  (0, -1, 0)),   # -Y
    ((-1, -1, 1), (2, 0, 0),  (0, 2, 0),  (0, 0, 1)),    # +Z
    ((1, -1, -1), (-2, 0, 0), (0, 2, 0),  (0, 0, -1)),   # -Z
]
QUAD_UV = ((0, 0), (1, 0), (1, 1), (0, 1))

# ── Декор крыши: база + наследники по виду геометрии ──────────────────────────
# Элемент крыши целится в ЦЕНТР блока; всё смещение от центра сидит в самом объекте (pivot),
# а не выбирается на месте — экземпляры одного вида одинаковы, случаен только выбор вида
# на блок (и вариант меша, если их несколько).
#
# Готовые меши (вышки) читаются ОДИН РАЗ при импорте и лежат приведёнными в self.variants:
# экземпляр на крыше — только перенос вершин, без разбора файла и без масштабирования.
# Приведение: равномерный масштаб под 10-юнитовый блок -> центр габарита в план (0,0) ->
# подошва в y=0. Начало координат экспортёра доверия не заслуживает (у radiotower оно смещено
# на юнит), а пивот обязан означать одно и то же для коробки и для меша. Масштаб РАВНОМЕРНЫЙ:
# нормали и тангенсы при нём не меняются, неравномерный потребовал бы обратной транспонированной.
#
# У меша СВОЙ сабмеш и свой материал, а имя вида = имя слота = имя материала (tower / telecom /
# radiotower — они в game/saved_scene/scene1/materials.json). Иначе вышка взяла бы материал фасада, то
# есть тайлящую программу LitTiled, а её uv лежат в [0,1] и тайлиться ей нечем. Цена — три
# материала в списке КАЖДОЙ сущности города. Процедурные виды остаются в слоте wall.
ROOF_ASSET_DIR = Path(__file__).resolve().parents[2] / "game" / "models"


def read_model(path):
    """Модель движка (.bin + _i.bin) -> (вершины, индексы), сабмеши слиты в один список.

    Индексы в файле ЛОКАЛЬНЫ для своего сабмеша — движок прибавляет к ним vertexOffset записи
    на исполнении (см. write_model). При слиянии в один список это надо сделать здесь, иначе
    второй сабмеш возьмёт вершины первого."""
    data = path.read_bytes()
    idx = path.with_name(path.stem + "_i.bin").read_bytes()
    n = struct.unpack_from("I", data, 0)[0]
    head = 4 + n * struct.calcsize(SUBMESH_ENTRY)
    stride = struct.calcsize(VERTEX_STRUCT)
    verts = [struct.unpack_from(VERTEX_STRUCT, data, head + stride * i)
             for i in range((len(data) - head) // stride)]
    raw = struct.unpack("<%dI" % (len(idx) // 4), idx)
    indices = []
    for k in range(n):
        vo, io_, _, ic, _ = struct.unpack_from(SUBMESH_ENTRY, data, 4 + k * 20)
        indices += [vo + j for j in raw[io_:io_ + ic]]
    return verts, indices


def fit_asset(verts, scale):
    """Масштаб -> центр габарита в план (0, 0) -> подошва в y = 0."""
    xs = [v[0] * scale for v in verts]
    ys = [v[1] * scale for v in verts]
    zs = [v[2] * scale for v in verts]
    dx = -(min(xs) + max(xs)) * 0.5
    dy = -min(ys)
    dz = -(min(zs) + max(zs)) * 0.5
    return [(x + dx, y + dy, z + dz) + v[3:] for x, y, z, v in zip(xs, ys, zs, verts)]


class RoofDecor:
    """База декора крыши: ЧТО и КОГДА ставится на блок. Геометрию даёт наследник.

    Здесь всё, что одинаково у любого вида: жеребьёвка (вес, лимит на здание, порог высоты),
    пивот, утопление в крышу, слот сабмеша и подпись для отчёта. Наследник добавляет ровно две
    вещи — свою геометрию и свои разыгрываемые параметры, — поэтому новый вид не трогает ни
    выбор, ни эмиссию, ни отчёт. Сам по себе базовый класс — это "пустая крыша".

    weight     — вес в жеребьёвке. Важнее, чем кажется: равновероятный выбор сажает готовый меш
                 больше чем на половину блоков, а меш стоит тысячи вершин против десятков у
                 коробки, и набор моделей начинает скакать вдвое по цене от одной монетки.
    limit      — сколько таких на ЗДАНИЕ; None = без ограничения.
    max_delta  — насколько этажей ниже САМОГО высокого возможного здания вид ещё допустим:
                 min_floors = FLOORS[1] - max_delta. None = на любой высоте. Вышке на приземистом
                 доме не место — она выше него самого, и город превращается в антенное поле.
    pivot      — (dx, dz) от центра блока.
    sink       — доля СОБСТВЕННОЙ высоты, на которую элемент утоплен в крышу; разыгрывается на
                 экземпляр. Одинаковая вышка на каждой второй крыше читается как копипаста.
    """

    slot = "wall"      # процедурные виды живут в слоте стены; свой слот только у готовых мешей

    def __init__(self, name, weight=1, limit=None, max_delta=None, pivot=(0.0, 0.0),
                 sink=(0.0, 0.0)):
        self.name = name
        self.weight = weight
        self.limit = limit
        self.min_floors = 0 if max_delta is None else FLOORS[1] - max_delta
        self.pivot = pivot
        self.sink = sink

    def load(self):
        """Чтение внешних данных на импорте модуля. Процедурным читать нечего."""
        return self

    def available(self, used, n_floors):
        """Годится ли вид этому зданию: лимит ещё не выбран и дом достаточно высок."""
        return (self.limit is None or used < self.limit) and n_floors >= self.min_floors

    def roll(self, rng):
        """Что разыгрывается на ЭКЗЕМПЛЯР -> словарь. Наследник дополняет своим.

        Словарь, а не кортеж: у видов разный набор случайного (вариант меша, доля отсечения),
        и позиционный кортеж пришлось бы расширять сразу во всех — ровно та беда, из-за
        которой таблица словарей и стала классами."""
        return {"sink": rng.uniform(*self.sink)}

    def own_height(self, params):
        """Своя высота элемента — на неё умножается доля утопления."""
        return 0.0

    def label(self, params):
        """Подпись для отчёта."""
        sink = params["sink"]
        return "%s-%.0f%%" % (self.name, 100.0 * sink) if sink > 0.0 else self.name

    def emit(self, mesh, params, cx, roof_y, cz):
        """Экземпляр на крышу блока. Берёт mesh целиком: слот у вида свой."""
        verts, indices = mesh[self.slot]
        origin = (cx + self.pivot[0],
                  roof_y - params["sink"] * self.own_height(params),
                  cz + self.pivot[1])
        self.geometry(verts, indices, origin, params)

    def geometry(self, verts, indices, origin, params):
        """Пустая крыша: ставить нечего."""


class BoxDecor(RoofDecor):
    """Набор коробок: (dx, dy, dz, hx, hy, hz) — смещение центра от пивота и полу-размеры.
    dy отсчитан от плоскости крыши, поэтому dy == hy ставит коробку РОВНО на неё."""

    def __init__(self, name, boxes=(), **kw):
        RoofDecor.__init__(self, name, **kw)
        self.boxes = boxes

    def own_height(self, params):
        return max((dy + hy for _, dy, _, _, hy, _ in self.boxes), default=0.0)

    def geometry(self, verts, indices, origin, params):
        ox, oy, oz = origin
        for dx, dy, dz, hx, hy, hz in self.boxes:
            emit_box(verts, indices, (ox + dx, oy + dy, oz + dz), (hx, hy, hz))


class CylinderDecor(RoofDecor):
    """Цилиндр: радиус, высота, сегментов кольца."""

    def __init__(self, name, radius, height, segments=24, **kw):
        RoofDecor.__init__(self, name, **kw)
        self.radius = radius
        self.height = height
        self.segments = segments

    def own_height(self, params):
        return self.height

    def geometry(self, verts, indices, origin, params):
        emit_cylinder(verts, indices, origin, self.radius, self.height, self.segments)


class PyramidDecor(RoofDecor):
    """Усечённая пирамида: radius — радиус описанной окружности основания, height — ПОЛНАЯ
    высота до вершины (без учёта отсечения), cut — доля, срезанная сверху.

    Одна доля задаёт и срез, и ширину площадки: сечение пирамиды на высоте t*H сужается ровно
    как (1-t), поэтому срезав сверху cut, получаем верх шириной base*cut. Отсечение
    разыгрывается на экземпляр — иначе все пирамиды города одинаковы, а весь смысл в разнобое."""

    def __init__(self, name, radius, height, cut=(0.3, 0.6), sides=4, **kw):
        RoofDecor.__init__(self, name, **kw)
        self.radius = radius
        self.height = height
        self.cut = cut
        self.sides = sides

    def roll(self, rng):
        params = RoofDecor.roll(self, rng)
        params["cut"] = rng.uniform(*self.cut)
        return params

    def own_height(self, params):
        return self.height * (1.0 - params["cut"])

    def label(self, params):
        return "%s-cut%.0f%%" % (self.name, 100.0 * params["cut"])

    def geometry(self, verts, indices, origin, params):
        cut = params["cut"]
        emit_frustum(verts, indices, origin, self.radius, self.radius * cut,
                     self.height * (1.0 - cut), self.sides)


class AssetDecor(RoofDecor):
    """Готовый меш из game/models. Несколько файлов = варианты ОДНОГО вида: шесть телекомов
    шестью видами перекосили бы жеребьёвку, поэтому вариант разыгрывается внутри.

    Меши читаются один раз при импорте и лежат приведёнными: экземпляр на крыше — только
    перенос вершин. Приведение: равномерный масштаб под 10-юнитовый блок -> центр габарита в
    план (0,0) -> подошва в y=0. Начало координат экспортёра доверия не заслуживает (у
    radiotower оно смещено на юнит), а пивот обязан означать одно и то же для коробки и меша.
    Масштаб РАВНОМЕРНЫЙ: нормали и тангенсы при нём не меняются, неравномерный потребовал бы
    пересчёта обратной транспонированной.

    Слот СВОЙ, одноимённый: имя вида = имя слота = имя материала (tower / telecom / radiotower
    в game/saved_scene/scene1/materials.json). Иначе меш взял бы материал фасада, то есть тайлящую
    программу LitTiled, а его uv лежат в [0,1] и тайлиться ей нечем."""

    def __init__(self, name, files, scale=1.0, **kw):
        RoofDecor.__init__(self, name, **kw)
        self.files = files
        self.scale = scale
        self.slot = name
        self.variants = ()

    def load(self):
        assert self.slot in SUBMESH_SLOTS, self.slot
        variants = []
        for rel in self.files:
            verts, indices = read_model(ROOF_ASSET_DIR / rel)
            fitted = fit_asset(verts, self.scale)
            # Высота нужна на месте: утопление задано ДОЛЕЙ от неё, а не юнитами — юниты
            # пришлось бы подбирать заново после каждой правки масштаба.
            variants.append((fitted, indices, max(v[1] for v in fitted)))
        self.variants = tuple(variants)
        return self

    def roll(self, rng):
        params = RoofDecor.roll(self, rng)
        params["variant"] = rng.randrange(max(1, len(self.variants)))
        return params

    def own_height(self, params):
        return self.variants[params["variant"]][2]

    def label(self, params):
        name = self.name
        if len(self.variants) > 1:
            name = "%s%d" % (name, params["variant"] + 1)
        sink = params["sink"]
        return "%s-%.0f%%" % (name, 100.0 * sink) if sink > 0.0 else name

    def geometry(self, verts, indices, origin, params):
        # Утопленная часть остаётся в буфере: вырезать её значило бы резать меш ассета, а он
        # приходит готовым. Она под крышей и ничего не стоит, кроме своих вершин.
        verts_src, indices_src, _ = self.variants[params["variant"]]
        ox, oy, oz = origin
        base = len(verts)
        for x, y, z, u, v, nx, ny, nz, tx, ty, tz in verts_src:
            verts.append((ox + x, oy + y, oz + z, u, v, nx, ny, nz, tx, ty, tz))
        indices += [base + i for i in indices_src]


ROOF_DECOR = [
    # Пустая крыша. Без лимита и без порога высоты: остальные виды упрутся в свои ограничения,
    # и добирать оставшиеся блоки должно быть чем.
    RoofDecor("none", weight=40),
    # Вертолётная площадка: очень низкий широкий цилиндр, пивот 0 — она и есть центр крыши.
    CylinderDecor("helipad", weight=30, limit=1, radius=4.0, height=0.35, segments=24),
    # Усечённая пирамида: венчает крышу, поэтому пивот 0 и лимит 1.
    PyramidDecor("pyramid", weight=15, limit=1, max_delta=25, radius=8.0, height=40.0, cut=(0.3, 0.6)),
    # Вентиляция: группа мелких коробок, сдвинута с центра — центр оставлен под площадку/вышку.
    BoxDecor("vents", weight=40, limit=3, pivot=(-2.0, 1.8), boxes=(
        (0.0, 0.9, 0.0, 0.9, 0.9, 0.9),
        (2.0, 0.7, 0.4, 0.7, 0.7, 0.7),
        (0.6, 1.3, -1.8, 0.5, 1.3, 0.5),
        (-1.6, 0.6, 1.2, 0.6, 0.6, 1.0),
    )),
    # Выход на крышу: коробка, вытянутая в плане — так она читается как лестничная пристройка,
    # а не как ещё один вентблок. У края блока по той же причине.
    BoxDecor("access", weight=40, limit=2, pivot=(2.4, -2.4), boxes=(
        (0.0, 1.4, 0.0, 2.4, 1.4, 1.3),
    )),
    # Вышки. max_delta пускает их только на верхнюю треть диапазона высот, поэтому вес поднят:
    # редкость теперь задаётся высотой дома, а не монеткой, и на подходящем доме вышка должна
    # появляться охотно.
    AssetDecor("tower", weight=30, limit=1, max_delta=15, sink=(0.0, 0.3),
               files=("tower/tower.bin",), scale=1.0 / 2.25),
    AssetDecor("radiotower", weight=40, limit=1, max_delta=15,
               files=("telecom/radiotower.bin",), scale=1.0 / 6.0),
    # Антенный пост невысок и стоит дёшево — ограничения по высоте ему не нужно.
    AssetDecor("telecom", weight=40, limit=3,
               files=tuple("telecom/telecom%d.bin" % i for i in range(1, 7))),
]

for _decor in ROOF_DECOR:
    _decor.load()


# Сосед по грани для каждой записи FACES, в клетках сетки (dx, dz). None = грань наружу всегда.
FACE_NEIGHBOUR = [(1, 0), (-1, 0), None, None, (0, 1), (0, -1)]

NEIGHBOUR_OFFSETS = [(dx, dz) for dx in (-1, 0, 1) for dz in (-1, 0, 1) if (dx, dz) != (0, 0)]


# ── Этап "форма": какие ячейки сетки заняты ───────────────────────────────────
def pick_footprint(rng):
    """BFS от случайной стартовой ячейки. Каждая ещё не посещённая соседка (включая диагональную)
    решается ОДНИМ броском: выбрана — попадает в форму и растит дальше, пропущена — рост в её
    сторону обрывается. visited помечается в ОБОИХ случаях, поэтому один и тот же кандидат не
    переспрашивается с другой стороны, а обход конечен. Форма всегда связная: каждая ячейка
    пришла от уже выбранного соседа."""
    start = (rng.randrange(GRID), rng.randrange(GRID))
    cells = {start}
    visited = {start}
    queue = deque([start])
    while queue:
        gx, gz = queue.popleft()
        for dx, dz in NEIGHBOUR_OFFSETS:
            nx, nz = gx + dx, gz + dz
            if not (0 <= nx < GRID and 0 <= nz < GRID) or (nx, nz) in visited:
                continue
            visited.add((nx, nz))
            if rng.random() < PICK_CHANCE:
                cells.add((nx, nz))
                queue.append((nx, nz))
    return cells


# ── Этап "высота" ─────────────────────────────────────────────────────────────
def assign_heights(rng):
    """Этажей у здания — одно число на ВСЕ блоки. Меряем в этажах, а не в юнитах: этаж —
    шаг, к которому обязаны привязаться ряды окон, поэтому высота обязана быть кратна ему."""
    return rng.randint(*FLOORS)


def tech_belt_floors(body_f, rng):
    """Этажи корпуса (индексы от его низа), с которых начинаются технические пояса.

    Количество случайно, но расстановка выбирается РАВНОМЕРНО среди всех допустимых сразу,
    без цикла отказов: k позиций с минимальным шагом D из M мест — это в точности k
    произвольных позиций из M-(k-1)(D-1), у которых i-я сдвинута на i*(D-1). Поэтому здание,
    куда влезает ровно один пояс, не приходится перебрасывать десятки раз.

    Границы: до цоколя и до крыши — те же TECH_MIN_GAP этажей, что и между поясами. Меряем
    до верха КОРПУСА, а венец (если он есть) идёт сверх того — запас только растёт."""
    T, G = TECH_FLOORS, TECH_MIN_GAP
    M = body_f - 2 * G - T + 1
    if M <= 0:
        return []
    D = G + T
    k = rng.randint(0, (M + D - 1) // D)
    if k == 0:
        return []
    picks = sorted(rng.sample(range(M - (k - 1) * (D - 1)), k))
    return [G + q + i * (D - 1) for i, q in enumerate(picks)]


# ── Этап "окна" ───────────────────────────────────────────────────────────────
def facade_cell():
    """Размер ячейки фасада: (ширина, высота проёма, высота ячейки). Ячейка ростом в этаж,
    проём внутри неё — доля."""
    wh = FLOOR_H * WINDOW_CELL_FLOORS
    return wh * window_tile_aspect(), wh, FLOOR_H


def facade_columns():
    """Ширина ячейки и число колонн на ВСЁ здание. Колонны считаются в глобальной решётке, а не
    внутри грани: тогда рисунок продолжается через стык двух блоков в одной плоскости."""
    cw = CELL / max(1, int(round(CELL / facade_cell()[0])))
    return cw, int(round(GRID * CELL / cw))


def assign_facade(rng, n_floors):
    """Этап "окна": маска фасада — своя на каждое направление грани.

    Хранится готовым множеством, а не считается хэшом на месте: фасадный квад это прямоугольник
    произвольного размера и положения, а не ячейка решётки, и проверить принадлежность ему
    функцией от координат нельзя — нужен список."""
    cw, ncols = facade_columns()
    masks = {}
    for f in (0, 1, 4, 5):
        blocked = set()
        target = WALL_QUAD_COVER * ncols * n_floors
        placed = 0
        while placed < target:
            w = rng.randint(*WALL_QUAD_COLS)
            h = rng.randint(*WALL_QUAD_ROWS)
            c0 = rng.randrange(max(1, ncols - w + 1))
            r0 = rng.randrange(max(1, n_floors - h + 1))
            for c in range(c0, min(c0 + w, ncols)):
                for r in range(r0, min(r0 + h, n_floors)):
                    blocked.add((c, r))
            placed += w * h          # с перекрытиями: реальное покрытие меньше цели

        win = set()
        for r in range(n_floors):
            c = rng.randrange(WINDOW_GAP_COLS[1])
            while c < ncols:
                run = rng.randint(*WINDOW_RUN_COLS)
                for k in range(c, min(c + run, ncols)):
                    if (k, r) not in blocked:
                        win.add((k, r))
                c += run + rng.randint(*WINDOW_GAP_COLS)
        masks[f] = win
    return {"seed": rng.randrange(1 << 30), "masks": masks, "cw": cw, "ncols": ncols}


def _mix(*vals):
    """Целочисленный хэш с ЛАВИНОЙ в конце (lowbias32).

    Финализатор обязателен: без него последнее подмешанное значение влияет только на младшие
    биты, и для соседних номеров групп монетка выпадает одинаково — грань уходит в окна целиком
    или не получает их вовсе."""
    h = 0
    for v in vals:
        h = ((h ^ (v & 0xFFFFFFFF)) * 16777619) & 0xFFFFFFFF
    h ^= h >> 16
    h = (h * 0x7FEB352D) & 0xFFFFFFFF
    h ^= h >> 15
    h = (h * 0x846CA68B) & 0xFFFFFFFF
    return (h ^ (h >> 16)) & 0xFFFFFFFF


def window_tile(seed, gx, gz, f, col, row):
    """Тайл конкретного окна — любой из 16. Строки листа НЕ семейства: на windows.jpg каждый
    тайл это отдельная квартира со своим светом, поэтому вариант берётся из всего листа.

    Выбор — ХЭШОМ от адреса окна, а не броском rng: эмиттеру меша не нужно тащить через себя
    генератор, а фасад воспроизводим при той же форме. seed разводит здания между собой."""
    n = _mix(seed, gx, gz, f, col, row) % (WINDOW_SHEET[0] * WINDOW_SHEET[1])
    tx, ty = n % WINDOW_SHEET[0], n // WINDOW_SHEET[0]
    du, dv = 1.0 / WINDOW_SHEET[0], 1.0 / WINDOW_SHEET[1]
    p = WINDOW_TILE_PAD
    return (tx * du + p, ty * dv + p, (tx + 1) * du - p, (ty + 1) * dv - p)


# ── Этап "вертикальные участки" ───────────────────────────────────────────────
def vertical_segments(n_floors, rng):
    """Список сегментов снизу вверх. kind "slab" — участок стены (коробка блока, раздутая
    наружу на extrude); kind "parapet" — стена крыши, кольцо по открытым рёбрам блока.

    grid=True помечает сегмент, который дальше получит сетку окон. Сейчас такой один (корпус),
    но поле не лишнее: цоколю обещан свой декор, и он придёт тем же механизмом с другим набором."""
    # Цоколь есть ВСЕГДА: вариант "цоколя нет" на модели неотличим от цоколя нулевой высоты,
    # то есть от ошибки. Нужен безцокольный силуэт — это отдельный вид сегментации, а не ноль.
    podium_f = rng.randint(int(math.ceil(PODIUM_MIN_H / FLOOR_H)), PODIUM_MAX_FLOORS)
    crown_f = rng.randint(*CROWN_FLOORS)
    body_f = n_floors - podium_f - crown_f

    y = 0.0
    segs = []
    if podium_f:
        segs.append({"name": "podium", "kind": "slab", "slot": "podium", "grid": False,
                     "y0": y, "y1": y + podium_f * FLOOR_H, "extrude": -PODIUM_INSET,
                     "floors": podium_f})
        y = segs[-1]["y1"]
    # Корпус разрезается техническими поясами на куски; каждый кусок — обычный сегмент стены
    # с сеткой окон, каждый пояс — сегмент со своим слотом (значит, своим материалом) и вносом.
    cursor = 0
    for start in tech_belt_floors(body_f, rng):
        segs.append({"name": "body", "kind": "slab", "slot": "wall", "grid": True,
                     "y0": y, "y1": y + (start - cursor) * FLOOR_H, "extrude": 0.0,
                     "floors": start - cursor})
        y = segs[-1]["y1"]
        segs.append({"name": "tech", "kind": "slab", "slot": "tech", "grid": False,
                     "y0": y, "y1": y + TECH_FLOORS * FLOOR_H, "extrude": -TECH_INSET,
                     "floors": TECH_FLOORS})
        y = segs[-1]["y1"]
        cursor = start + TECH_FLOORS
    segs.append({"name": "body", "kind": "slab", "slot": "wall", "grid": True,
                 "y0": y, "y1": y + (body_f - cursor) * FLOOR_H, "extrude": 0.0,
                 "floors": body_f - cursor})
    y = segs[-1]["y1"]
    if crown_f:
        segs.append({"name": "crown", "kind": "slab", "slot": "wall", "grid": False,
                     "y0": y, "y1": y + crown_f * FLOOR_H, "extrude": 0.0, "floors": crown_f})
        y = segs[-1]["y1"]
    segs.append({"name": "parapet", "kind": "parapet", "slot": "wall", "grid": False,
                 "y0": y, "y1": y + PARAPET_H, "extrude": 0.0, "floors": 0})
    return segs


# ── Этап "крыша": по элементу на блок ─────────────────────────────────────────
def assign_roofs(cells, n_floors, rng):
    """Ровно один элемент на блок. Выбор случаен среди тех видов, что этому зданию доступны:
    исчерпавший лимит или не прошедший по высоте не «пропускает ход» (иначе крыша молча
    пустела бы), а выбывает из жеребьёвки, и блок получает что-то другое. "none" без
    ограничений — им и добирается остаток.

    -> {ячейка: (вид, разыгранные параметры экземпляра)}."""
    used = {d.name: 0 for d in ROOF_DECOR}
    roofs = {}
    for cell in sorted(cells):
        free = [d for d in ROOF_DECOR if d.available(used[d.name], n_floors)]
        pick = rng.choices(free, [d.weight for d in free])[0]
        used[pick.name] += 1
        roofs[cell] = (pick, pick.roll(rng))
    return roofs


# ── Меш ───────────────────────────────────────────────────────────────────────
def emit_face(verts, indices, f, center, half, uv=(0.0, 0.0, 1.0, 1.0)):
    """Одна грань по канону движкового куба: v-down развёртка, тангенс = направление U.

    center/half — коробка ИМЕННО этой грани. uv — подпрямоугольник текстуры (u0, v0, u1, v1),
    где (u0, v0) — ВЕРХНИЙ левый угол в текстурных координатах: этим же аргументом дальше
    выбирается тайл окна из листа вариантов. По умолчанию грань берёт текстуру целиком."""
    c, U, V, N = FACES[f]
    # Тангенс — направление U в МИРОВЫХ пропорциях (грани неквадратные, half по осям разный).
    tx, ty, tz = U[0] * half[0], U[1] * half[1], U[2] * half[2]
    tl = (tx * tx + ty * ty + tz * tz) ** 0.5
    if tl > 0.0:
        tx, ty, tz = tx / tl, ty / tl, tz / tl

    base = len(verts)
    for uq, vq in QUAD_UV:
        px = (c[0] + uq * U[0] + vq * V[0]) * half[0] + center[0]
        py = (c[1] + uq * U[1] + vq * V[1]) * half[1] + center[1]
        pz = (c[2] + uq * U[2] + vq * V[2]) * half[2] + center[2]
        u = uv[0] + uq * (uv[2] - uv[0])
        v = uv[1] + (1.0 - vq) * (uv[3] - uv[1])
        verts.append((px, py, pz, u, v, N[0], N[1], N[2], tx, ty, tz))
    indices += [base + 0, base + 1, base + 2, base + 0, base + 2, base + 3]


def emit_box(verts, indices, center, half, skip=(3,)):
    """Коробка целиком. По умолчанию пропускается -Y: элементы крыши стоят НА ней, их дно
    закрыто и совпало бы с ней плоскостью."""
    for f in range(6):
        if f not in skip:
            emit_face(verts, indices, f, center, half)


def emit_cylinder(verts, indices, center, r, h, seg):
    """Цилиндр без дна (стоит на крыше): боковина + верхняя крышка веером.
    Обмотка и развёртка — тот же канон, что у граней: CCW наружу, v-down, тангенс вдоль U."""
    cx, cy, cz = center
    ring = []
    for i in range(seg):
        a = 2.0 * math.pi * i / seg
        ring.append((math.cos(a), math.sin(a)))

    for i in range(seg):
        c0, s0 = ring[i]
        c1, s1 = ring[(i + 1) % seg]
        u0, u1 = i / seg, (i + 1) / seg
        base = len(verts)
        # Порядок низ-a0, верх-a0, верх-a1, низ-a1 даёт CCW снаружи (проверено check_mesh).
        quad = (((c0, s0), 0.0, u0, 1.0), ((c0, s0), h, u0, 0.0),
                ((c1, s1), h, u1, 0.0), ((c1, s1), 0.0, u1, 1.0))
        for (cs, sn), y, u, v in quad:
            # Нормаль радиальная, тангенс — вдоль растущего u, то есть по касательной кольца.
            verts.append((cx + r * cs, cy + y, cz + r * sn, u, v,
                          cs, 0.0, sn, -sn, 0.0, cs))
        indices += [base + 0, base + 1, base + 2, base + 0, base + 2, base + 3]

    # Крышка: веер от центра. Обход по УБЫВАЮЩЕМУ углу — +Y-грань канона идёт +X, затем -Z.
    top = len(verts)
    verts.append((cx, cy + h, cz, 0.5, 0.5, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0))
    for cs, sn in ring:
        verts.append((cx + r * cs, cy + h, cz + r * sn,
                      0.5 + cs * 0.5, 0.5 + sn * 0.5, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0))
    for i in range(seg):
        indices += [top, top + 1 + (i + 1) % seg, top + 1 + i]


def emit_frustum(verts, indices, center, r0, r1, h, seg):
    """Усечённая пирамида (seg=4) или конус: боковые трапеции + верхняя крышка веером.

    r0/r1 — радиусы ОПИСАННОЙ окружности снизу и сверху. Кольцо повёрнуто на pi/seg, иначе у
    четырёхгранника грани смотрят по диагоналям блока, а не по его сторонам.
    Нормаль боковой грани НАКЛОНЕНА: горизонтальная часть наружу, вертикальная — на уклон
    (r0-r1)/h. Без наклона скат светился бы как вертикальная стена, а он смотрит в небо.
    Дна нет: элемент стоит на крыше, и дно совпало бы с ней плоскостью."""
    cx, cy, cz = center
    phase = math.pi / seg
    ring = [(math.cos(phase + 2.0 * math.pi * i / seg), math.sin(phase + 2.0 * math.pi * i / seg))
            for i in range(seg)]

    for i in range(seg):
        c0, s0 = ring[i]
        c1, s1 = ring[(i + 1) % seg]
        # Наружу у грани смотрит биссектриса, а не вершина кольца.
        mx, mz = (c0 + c1) * 0.5, (s0 + s1) * 0.5
        ml = math.hypot(mx, mz) or 1.0
        nx, ny, nz = mx / ml * h, r0 - r1, mz / ml * h
        nl = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
        nx, ny, nz = nx / nl, ny / nl, nz / nl
        tx, tz = c1 - c0, s1 - s0
        tl = math.hypot(tx, tz) or 1.0
        tx, tz = tx / tl, tz / tl

        base = len(verts)
        # Тот же обход, что у цилиндра: низ-a0, верх-a0, верх-a1, низ-a1 = CCW снаружи.
        quad = ((c0, s0, r0, 0.0, 0.0, 1.0), (c0, s0, r1, h, 0.0, 0.0),
                (c1, s1, r1, h, 1.0, 0.0), (c1, s1, r0, 0.0, 1.0, 1.0))
        for cs, sn, r, y, u, v in quad:
            verts.append((cx + r * cs, cy + y, cz + r * sn, u, v, nx, ny, nz, tx, 0.0, tz))
        indices += [base + 0, base + 1, base + 2, base + 0, base + 2, base + 3]

    # Крышка: веер от центра, обход по УБЫВАЮЩЕМУ углу — как у цилиндра.
    top = len(verts)
    verts.append((cx, cy + h, cz, 0.5, 0.5, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0))
    for cs, sn in ring:
        verts.append((cx + r1 * cs, cy + h, cz + r1 * sn,
                      0.5 + cs * 0.5, 0.5 + sn * 0.5, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0))
    for i in range(seg):
        indices += [top, top + 1 + (i + 1) % seg, top + 1 + i]


def face_box(f, cx, cz, hspan, vspan, depth, offset=0.0):
    """center/half коробки, приклеенной к грани f блока с центром (cx, cz) в плане.

    hspan/vspan — МИРОВЫЕ интервалы прямоугольника (hspan по Z у граней +-X, по X у +-Z);
    offset двигает плоскость стены наружу (вынос сегмента), depth — толщина от неё: >0 наружу,
    <0 внутрь, 0 = плоский квад. Этим же хелпером дальше режутся проёмы и откосы окон — он
    и есть перевод "прямоугольник на стене" в коробку канона."""
    sign = 1.0 if f in (0, 4) else -1.0
    h0, h1 = hspan
    v0, v1 = vspan
    hc, hh = (h0 + h1) * 0.5, (h1 - h0) * 0.5
    nc = sign * (CELL * 0.5 + offset + depth * 0.5) + (cx if f in (0, 1) else cz)
    nh = abs(depth) * 0.5
    if f in (0, 1):
        return (nc, (v0 + v1) * 0.5, hc), (nh, (v1 - v0) * 0.5, hh)
    return (hc, (v0 + v1) * 0.5, nc), (hh, (v1 - v0) * 0.5, nh)


def slab_bounds(seg, cells, gx, gz):
    """Мировые габариты сегмента блока в плане.

    Вынос/внос сегмента применяется ТОЛЬКО на внешних сторонах: на общей с соседом стороне
    сегмент обязан быть вровень с границей блока. Иначе габариты соседей расходятся, и на
    стыке появляется наслоение (сегмент шире) или сквозная щель (уже) — а отсечение общей
    грани при этом продолжает считать её закрытой."""
    span = GRID * CELL
    cx = gx * CELL + CELL * 0.5 - span * 0.5
    cz = gz * CELL + CELL * 0.5 - span * 0.5
    e = {}
    for f, nb in enumerate(FACE_NEIGHBOUR):
        if nb is not None:
            e[f] = 0.0 if (gx + nb[0], gz + nb[1]) in cells else seg["extrude"]
    return (cx - CELL * 0.5 - e[1], cx + CELL * 0.5 + e[0],
            cz - CELL * 0.5 - e[5], cz + CELL * 0.5 + e[4], cx, cz, e)


def interval_diff(a, b):
    """a без b для отрезков: 0..2 куска."""
    out = []
    if b[0] > a[0]:
        out.append((a[0], min(b[0], a[1])))
    if b[1] < a[1]:
        out.append((max(b[1], a[0]), a[1]))
    return [(p, q) for p, q in out if q - p > 1e-6]


def emit_face_panels(verts, indices, f, center, half, target=None, tiled=False):
    """Грань коробки, повторяющая текстуру с шагом ~target (по умолчанию — этаж).

    ДВА способа повторить, и выбор между ними — это выбор ШЕЙДЕРА материала:
      tiled=False — резать на панели, каждая со своим полным [0,1]. Единственный вариант для
        движкового Lit: sampleAtlas считает uv*scale+offset без frac, и uv>1 уехал бы в соседнюю
        текстуру атласа. Сторона КОРОЧЕ панели берёт ДОЛЮ текстуры, а не сжимает в себя всю —
        иначе полоска-заплатка показывает ту же картинку в другом масштабе, и это видно разрывом.
      tiled=True — один квад с uv 0..(размер/шаг). Требует игровой программы LitTiled
        (game/shaders/building_surface.hlsl), которая берёт frac(uv) сама. Картинка та же,
        вершин в разы меньше. Доля короткой стороны получается сама собой: su<tu -> u1<1."""
    tu, tv = target or (facade_cell()[0], PANEL_FLOORS * FLOOR_H)
    c, U, V, N = FACES[f]
    au = next(i for i in range(3) if U[i])
    av = next(i for i in range(3) if V[i])
    su, sv = 2.0 * half[au], 2.0 * half[av]

    if tiled:
        emit_face(verts, indices, f, center, half, (0.0, 0.0, su / tu, sv / tv))
        return

    nu = max(1, int(round(su / tu))) if su >= tu else 1
    nv = max(1, int(round(sv / tv))) if sv >= tv else 1
    fu = su / tu if su < tu else 1.0
    fv = sv / tv if sv < tv else 1.0

    for i in range(nu):
        for k in range(nv):
            sc, sh = list(center), list(half)
            sc[au] = center[au] - half[au] + su * (i + 0.5) / nu
            sh[au] = su * 0.5 / nu
            sc[av] = center[av] - half[av] + sv * (k + 0.5) / nv
            sh[av] = sv * 0.5 / nv
            emit_face(verts, indices, f, tuple(sc), tuple(sh), (0.0, 0.0, fu, fv))


def emit_box_panels(verts, indices, center, half, skip=(3,), tiled=False):
    """Коробка, все грани которой повторяют текстуру, — для элементов СТЕНЫ (парапет).
    Элементы крыши остаются на emit_box: у пропа текстура ложится на грань один раз."""
    for f in range(6):
        if f not in skip:
            emit_face_panels(verts, indices, f, center, half, tiled=tiled)


def merge_rects(solid, nrows, ncols):
    """Жадное слияние сплошных ячеек в максимальные прямоугольники -> (row, col, nrow, ncol).

    Смысл ровно в том, что фасадные квады генерации И ЕСТЬ прямоугольники: регион 5x8, который
    гарантированно стена, уходит одним квадом вместо сорока. Жадности хватает — оптимальное
    покрытие прямоугольниками тут не нужно, а стоит заметно дороже."""
    used = [[False] * ncols for _ in range(nrows)]
    out = []
    for r in range(nrows):
        for cbeg in range(ncols):
            if used[r][cbeg] or not solid[r][cbeg]:
                continue
            w = 0
            while cbeg + w < ncols and solid[r][cbeg + w] and not used[r][cbeg + w]:
                w += 1
            h = 1
            while r + h < nrows and all(solid[r + h][cbeg + i] and not used[r + h][cbeg + i]
                                        for i in range(w)):
                h += 1
            for rr in range(r, r + h):
                for cc in range(cbeg, cbeg + w):
                    used[rr][cc] = True
            out.append((r, cbeg, h, w))
    return out


def window_grid(f, hspan, vspan, fac):
    """Маска окон пролёта -> (win[row][col], ncols, nrows, cw, col0).

    Колонны и этажи считаются в ГЛОБАЛЬНОЙ решётке здания (col0 — от мирового начала пролёта),
    поэтому маска не знает и не должна знать, чем эмитируется стена под ней: разрезана она по
    ячейкам или слита в общий контур, рисунок один и тот же и через бывший стык продолжается."""
    _, wh, ch = facade_cell()
    cw_nom, _ = facade_columns()
    width = hspan[1] - hspan[0]
    ncols = max(1, int(round(width / cw_nom)))
    cw = width / ncols                       # подгоняем под пролёт: окно не должно резаться краем
    nrows = max(1, int(round((vspan[1] - vspan[0]) / ch)))
    # Пролёт, не кратный ячейке (заплатка на стыке блоков), из глобальной решётки выпадает —
    # его колонны не совпали бы с колоннами соседей, поэтому он остаётся сплошной стеной.
    aligned = abs(cw - cw_nom) < 1e-6
    col0 = int(round((hspan[0] + GRID * CELL * 0.5) / cw_nom))
    mask = fac["masks"].get(f, ())
    win = [[aligned and (col0 + col, int(round((vspan[0] + row * ch) / FLOOR_H))) in mask
            for col in range(ncols)] for row in range(nrows)]
    return win, ncols, nrows, cw, col0


def emit_windows(mesh, f, cx, cz, hspan, vspan, offset, fac, fixed):
    """Накладной слой окон на пролёт стены — стену не трогает вовсе.

    Вариант тайла берётся по ЯЧЕЙКЕ, над которой стоит окно, а ячейка вычисляется из номера
    колонны: колонн в ячейке целое число (facade_columns строит cw делением CELL), поэтому на
    слитом прогоне через несколько блоков разбиение по вариантам остаётся тем же, что было при
    поячеечной эмиссии. fixed — номер ячейки по оси, перпендикулярной прогону."""
    win_v, win_i = mesh["windows"]
    _, wh, ch = facade_cell()
    cw_nom, _ = facade_columns()
    per_cell = max(1, int(round(CELL / cw_nom)))
    win, ncols, nrows, cw, col0 = window_grid(f, hspan, vspan, fac)
    sill = ch * WINDOW_SILL_FLOORS

    for row in range(nrows):
        y0 = vspan[0] + row * ch + sill
        for col in range(ncols):
            if not win[row][col]:
                continue
            g = col0 + col
            cell_i, local = divmod(g, per_cell)
            gx, gz = (fixed, cell_i) if f in (0, 1) else (cell_i, fixed)
            center, half = face_box(f, cx, cz,
                                    (hspan[0] + col * cw, hspan[0] + (col + 1) * cw),
                                    (y0, y0 + wh), 0.0, offset + WINDOW_LIFT)
            emit_face(win_v, win_i, f, center, half,
                      window_tile(fac["seed"], gx, gz, f, local, row))


def emit_facade(mesh, seg, f, cx, cz, hspan, vspan, offset, fac, gx, gz):
    """Пролёт стены сегмента с сеткой -> стена + окна.

    WINDOW_OVERLAY: стена уходит ОДНИМ тайлящимся квадом на весь пролёт, окна кладутся поверх
    неё приподнятыми. Стена под окном никому не видна и ничего не стоит — платим только за само
    окно (4 вершины), а не за обход дырки лентами и прямоугольниками.

    Иначе — стена ВЫРЕЗАЕТСЯ: ячейка ростом в этаж делится на подоконную часть, проём и
    надоконную; сплошные ячейки собираются в максимальные прямоугольники (merge_rects), а
    подоконные и надоконные полосы — в ленты по длине группы окон. Компланарности нет, depth
    bias не нужен, но построчная структура стоит вершин."""
    wall_v, wall_i = mesh[seg["slot"]]
    win_v, win_i = mesh["windows"]
    tiled = seg["slot"] in TILED_SLOTS

    _, wh, ch = facade_cell()
    sill = ch * WINDOW_SILL_FLOORS
    head = ch - sill - wh
    fs, fw = sill / ch, wh / ch              # доли ячейки по вертикали
    win, ncols, nrows, cw, col0 = window_grid(f, hspan, vspan, fac)

    def hs(col, n=1):
        return (hspan[0] + col * cw, hspan[0] + (col + n) * cw)

    if WINDOW_OVERLAY:
        center, half = face_box(f, cx, cz, hspan, vspan, 0.0, offset)
        emit_face_panels(wall_v, wall_i, f, center, half, tiled=tiled)
        emit_windows(mesh, f, cx, cz, hspan, vspan, offset, fac,
                     gx if f in (0, 1) else gz)
        return
    else:
        for r, c, nr, nc in merge_rects([[not w for w in row] for row in win], nrows, ncols):
            y0 = vspan[0] + r * ch
            center, half = face_box(f, cx, cz, hs(c, nc), (y0, y0 + nr * ch), 0.0, offset)
            emit_face(wall_v, wall_i, f, center, half, (0.0, 0.0, float(nc), float(nr)))

    for row in range(nrows):
        y0 = vspan[0] + row * ch
        col = 0
        while col < ncols:
            if not win[row][col]:
                col += 1
                continue
            n = 1
            while col + n < ncols and win[row][col + n]:
                n += 1

            center, half = face_box(f, cx, cz, hs(col, n), (y0, y0 + sill), 0.0, offset)
            emit_face(wall_v, wall_i, f, center, half, (0.0, 1.0 - fs, float(n), 1.0))
            if head > 1e-6:
                center, half = face_box(f, cx, cz, hs(col, n),
                                        (y0 + sill + wh, y0 + ch), 0.0, offset)
                emit_face(wall_v, wall_i, f, center, half,
                          (0.0, 0.0, float(n), 1.0 - fs - fw))

            for k in range(n):
                center, half = face_box(f, cx, cz, hs(col + k),
                                        (y0 + sill, y0 + sill + wh), 0.0, offset)
                emit_face(win_v, win_i, f, center, half,
                          window_tile(fac["seed"], gx, gz, f, col + k, row))
            col += n


def cell_center(g):
    return g * CELL + CELL * 0.5 - GRID * CELL * 0.5


def silhouette_runs(cells, f, extrude):
    """Максимальные ПРОГОНЫ ячеек, открытых наружу гранью f -> (cx, cz, hspan) на прогон.

    Прогон рвётся там, где грань закрыта соседним блоком, поэтому контур уровня выходит ровно
    тот же, что у поячеечной эмиссии, — просто одним квадом на сторону вместо N.
    Торцы прогона вынесены так же, как вынесена сама стена: сосед за торцом отсутствует —
    значит там наружная сторона, и не вынести её значило бы разойтись с перпендикулярной
    стеной ровно на вынос сегмента."""
    nbx, nbz = FACE_NEIGHBOUR[f]
    span = GRID * CELL
    for fixed in range(GRID):
        opened = []
        for k in range(GRID):
            gx, gz = (fixed, k) if f in (0, 1) else (k, fixed)
            if (gx, gz) in cells and (gx + nbx, gz + nbz) not in cells:
                opened.append(k)
        k = 0
        while k < len(opened):
            j = k
            while j + 1 < len(opened) and opened[j + 1] == opened[j] + 1:
                j += 1
            beg, end = opened[k], opened[j]
            lo = beg * CELL - span * 0.5
            hi = (end + 1) * CELL - span * 0.5
            before = (fixed, beg - 1) if f in (0, 1) else (beg - 1, fixed)
            after = (fixed, end + 1) if f in (0, 1) else (end + 1, fixed)
            if before not in cells:
                lo -= extrude
            if after not in cells:
                hi += extrude
            if f in (0, 1):
                yield cell_center(fixed), (lo + hi) * 0.5, (lo, hi)
            else:
                yield (lo + hi) * 0.5, cell_center(fixed), (lo, hi)
            k = j + 1


def fixed_cell(f, cx, cz):
    """Номер ячейки по оси, ПЕРПЕНДИКУЛЯРНОЙ прогону: у граней +-X это gx, у +-Z — gz."""
    c = cx if f in (0, 1) else cz
    return int(round((c + GRID * CELL * 0.5 - CELL * 0.5) / CELL))


def emit_shell(mesh, seg, cells, fac, cap_bottom, cap_top):
    """Уровень целиком: по квадру на прогон контура + крышки прямоугольниками.

    Крышки СЛИТЫ по сетке ячеек (merge_rects) и выноса не получают: ненулевой вынос бывает
    только у подошвы цоколя, а она в земле. Всё, что видно (плоскость крыши, потолок свеса над
    цоколем), лежит у сегмента с выносом 0, где сетка и стена совпадают."""
    verts, indices = mesh[seg["slot"]]
    vspan = (seg["y0"], seg["y1"])
    # Сегмент нулевой высоты законен (техпояс, вставший вплотную к венцу, съедает корпус целиком),
    # и стены у него нет — квад вышел бы вырожденным. Крышки при этом нужны: ими закрывается
    # полка пояса. Панельный режим приходит к тому же сам — панелей в нулевой высоте не выходит.
    for f in SIDE_FACES if vspan[1] - vspan[0] > 1e-6 else ():
        for cx, cz, hspan in silhouette_runs(cells, f, seg["extrude"]):
            center, half = face_box(f, cx, cz, hspan, vspan, 0.0, seg["extrude"])
            emit_face(verts, indices, f, center, half)
            # Окна — слой ПОВЕРХ стены, к её разбиению не привязанный: на слитом прогоне они
            # ложатся так же, как ложились на четыре отдельные ячейки, потому что маска живёт
            # в глобальной решётке колонн, а не внутри пролёта.
            if seg["grid"]:
                emit_windows(mesh, f, cx, cz, hspan, vspan, seg["extrude"], fac,
                             fixed_cell(f, cx, cz))

    span = GRID * CELL
    solid = [[(gx, gz) in cells for gx in range(GRID)] for gz in range(GRID)]
    for f, needed in ((2, cap_top), (3, cap_bottom)):
        if not needed:
            continue
        for r, c, nr, nc in merge_rects(solid, GRID, GRID):
            x0, x1 = c * CELL - span * 0.5, (c + nc) * CELL - span * 0.5
            z0, z1 = r * CELL - span * 0.5, (r + nr) * CELL - span * 0.5
            center = ((x0 + x1) * 0.5, (vspan[0] + vspan[1]) * 0.5, (z0 + z1) * 0.5)
            half = ((x1 - x0) * 0.5, (vspan[1] - vspan[0]) * 0.5, (z1 - z0) * 0.5)
            emit_face(verts, indices, f, center, half)


def emit_parapet_shell(mesh, seg, cells):
    """Парапет по тому же контуру: коробка на прогон вместо коробки на ячейку."""
    verts, indices = mesh[seg["slot"]]
    for f in SIDE_FACES:
        for cx, cz, hspan in silhouette_runs(cells, f, 0.0):
            center, half = face_box(f, cx, cz, hspan, (seg["y0"], seg["y1"]), -PARAPET_T)
            emit_box(verts, indices, center, half)


def emit_slab(mesh, seg, cells, gx, gz, cap_bottom, cap_top, fac):
    """Участок стены = коробка блока с посторонним выносом на внешних сторонах.

    Общая с соседом грань эмитится НЕ целиком и не пропускается целиком: рисуется та её часть,
    которую сосед своим сегментом не закрыл. Совпали габариты (обычный случай) — разность пуста
    и грань уходит вся, как раньше. Разошлись — закрывается ровно щель.

    У сегмента с сеткой (grid) боковые панели — это ОКНА: те же квады, но в своём сабмеше и с
    тайлом вместо полного [0,1]. Крышки окнами не становятся никогда: это горизонтальные полки."""
    x0, x1, z0, z1, cx, cz, e = slab_bounds(seg, cells, gx, gz)
    vspan = (seg["y0"], seg["y1"])
    verts, indices = mesh[seg["slot"]]
    side_v, side_i = verts, indices
    tiled = seg["slot"] in TILED_SLOTS

    for f, nb in enumerate(FACE_NEIGHBOUR):
        if nb is None:
            if not (cap_top if f == 2 else cap_bottom):
                continue
            center = ((x0 + x1) * 0.5, (seg["y0"] + seg["y1"]) * 0.5, (z0 + z1) * 0.5)
            half = ((x1 - x0) * 0.5, (seg["y1"] - seg["y0"]) * 0.5, (z1 - z0) * 0.5)
            emit_face_panels(verts, indices, f, center, half, tiled=tiled)
            continue

        hspan = (z0, z1) if f in (0, 1) else (x0, x1)
        pieces = [hspan]
        nbc = (gx + nb[0], gz + nb[1])
        if nbc in cells:
            nb0, nb1, nb2, nb3, _, _, _ = slab_bounds(seg, cells, *nbc)
            pieces = interval_diff(hspan, (nb2, nb3) if f in (0, 1) else (nb0, nb1))
        for piece in pieces:
            center, half = face_box(f, cx, cz, piece, vspan, 0.0, e[f])
            if seg["grid"]:
                emit_facade(mesh, seg, f, cx, cz, piece, vspan, e[f], fac, gx, gz)
            else:
                emit_face_panels(side_v, side_i, f, center, half, tiled=tiled)


def emit_parapet(verts, indices, seg, cells, gx, gz):
    """Стена крыши: по коробке на каждое ОТКРЫТОЕ ребро блока, снаружи вровень со стеной,
    толщиной внутрь. На углу две перпендикулярные коробки пересекаются — так и надо,
    иначе в углу осталась бы дыра."""
    span = GRID * CELL
    cx = gx * CELL + CELL * 0.5 - span * 0.5
    cz = gz * CELL + CELL * 0.5 - span * 0.5
    for f, nb in enumerate(FACE_NEIGHBOUR):
        if nb is None or (gx + nb[0], gz + nb[1]) in cells:
            continue
        # Пролёт грани — МИРОВОЙ (как того требует face_box): на всю ширину блока, чтобы
        # соседние отрезки парапета сходились на общей границе в непрерывный периметр.
        hspan = (cz - CELL * 0.5, cz + CELL * 0.5) if f in (0, 1) else (cx - CELL * 0.5, cx + CELL * 0.5)
        center, half = face_box(f, cx, cz, hspan, (seg["y0"], seg["y1"]), -PARAPET_T)
        emit_box_panels(verts, indices, center, half, tiled=True)   # парапет — слот wall


def build_mesh(cells, segs, roofs, fac):
    """Ячейки + вертикальные участки -> буферы ПО СЛОТАМ сабмешей."""
    mesh = {slot: ([], []) for slot in SUBMESH_SLOTS}
    slabs = [i for i, sg in enumerate(segs) if sg["kind"] == "slab"]
    roof_y = segs[-1]["y0"]

    for i, seg in enumerate(segs):
        # Крышка нужна там, где сегмент ВЫСТУПАЕТ за соседний по высоте (полка цоколя),
        # и на самых краях: снизу — подошва, сверху — плоскость крыши.
        caps = (True, True)
        if seg["kind"] != "parapet":
            k = slabs.index(i)
            below = segs[slabs[k - 1]]["extrude"] if k > 0 else None
            above = segs[slabs[k + 1]]["extrude"] if k + 1 < len(slabs) else None
            caps = (below is None or seg["extrude"] > below,
                    above is None or seg["extrude"] > above)

        if SIMPLE_SHELL:
            # Уровень эмитится ЦЕЛИКОМ, а не по ячейке: слияние блоков в общий контур — это и
            # есть весь смысл режима, поячеечный обход его бы не увидел.
            if seg["kind"] == "parapet":
                emit_parapet_shell(mesh, seg, cells)
            else:
                emit_shell(mesh, seg, cells, fac, caps[0], caps[1])
            continue

        for (gx, gz) in sorted(cells):
            if seg["kind"] == "parapet":
                verts, indices = mesh[seg["slot"]]
                emit_parapet(verts, indices, seg, cells, gx, gz)
            else:
                emit_slab(mesh, seg, cells, gx, gz, caps[0], caps[1], fac)

    for (gx, gz) in sorted(cells):
        decor, params = roofs[(gx, gz)]
        decor.emit(mesh, params, cell_center(gx), roof_y, cell_center(gz))
    return mesh


def write_model(stem, mesh):
    """Пишет ВСЕ слоты сабмешами в порядке их номеров, пустые в том числе.

    Индексы в файле ЛОКАЛЬНЫ для своего сабмеша: движок кладёт vertexOffset записи в
    vertex_offset индирект-команды (IndirectDataModule.cpp:65), и он прибавляется к каждому
    индексу. Рабазировать их на общий буфер нельзя — сместится дважды.

    Почему ВСЕ, а не только занятые. Пропуск номера отправил бы всё, что за ним, в
    "material_index out of range" — это старая причина, и она требовала лишь затыкать дырки.
    Новая сильнее: число сабмешей модели — это длина списка материалов у КАЖДОЙ сущности сцены
    (BatchBuilder.cpp сверяет их). Пока оно зависело от того, что выпало на крышах, перезапуск
    генератора молча ломал уже сохранённую сцену. Теперь оно константа — len(SUBMESH_SLOTS).
    Пустая запись (0 вершин, 0 индексов) не рисует ничего и до узла батча не доходит."""
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    vpath = OUT_DIR / (stem + "_v.bin")
    ipath = OUT_DIR / (stem + "_i.bin")

    parts = sorted((SUBMESH_SLOTS[n], n, mesh[n]) for n in mesh)

    entries, all_verts, all_indices = [], [], []
    for mat_index, name, (verts, indices) in parts:
        entries.append((len(all_verts), len(all_indices), len(verts), len(indices), mat_index))
        all_verts += verts
        all_indices += indices

    with open(vpath, "wb") as fv:
        fv.write(struct.pack("I", len(entries)))
        for e in entries:
            fv.write(struct.pack(SUBMESH_ENTRY, *e))
        for v in all_verts:
            fv.write(struct.pack(VERTEX_STRUCT, *v))
    with open(ipath, "wb") as fi:
        fi.write(struct.pack("<%dI" % len(all_indices), *all_indices))
    return vpath, ipath, parts


def build_one(rng):
    """Одна модель: форма -> высота -> участки -> крыши -> фасад -> буферы сабмешей."""
    cells = pick_footprint(rng)
    n_floors = assign_heights(rng)
    segs = vertical_segments(n_floors, rng)
    roofs = assign_roofs(cells, n_floors, rng)
    fac = assign_facade(rng, n_floors)
    return cells, n_floors, segs, roofs, fac, build_mesh(cells, segs, roofs, fac)


def main():
    rng = random.Random()
    px = jpeg_size(str(Path(__file__).resolve().parents[2] / WINDOW_TEXTURE))
    cw, wh, ch = facade_cell()
    print("mode: %s" % ("SIMPLE_SHELL - merged silhouette, no windows, no tiling"
                        if SIMPLE_SHELL else "panels + window overlay"))
    print("windows: %s %s, sheet %dx%d, tile aspect %.2f"
          % (Path(WINDOW_TEXTURE).name, "%dx%d" % px if px else "(not found)",
             WINDOW_SHEET[0], WINDOW_SHEET[1], window_tile_aspect()))
    print("         cell %.2f x %.2f (opening %.2f x %.2f), %d per block face, %d cols per building"
          % (cw, ch, cw, wh, round(CELL / cw), facade_columns()[1]))
    print("")

    for n in range(1, MODEL_COUNT + 1):
        stem = "%s%d" % (MODEL_NAME, n)
        cells, n_floors, segs, roofs, fac, mesh = build_one(rng)
        vpath, ipath, parts = write_model(stem, mesh)

        win = len(fac["masks"][0])
        verts = sum(len(v) for _, _, (v, _) in parts)
        print("%-11s %d cells  %2d floors (%.1f units)  %5d verts  windows %.0f%% of +X face"
              % (stem, len(cells), n_floors, n_floors * FLOOR_H, verts,
                 100.0 * win / (fac["ncols"] * n_floors)))
        print("            roofs   " + " ".join(
            sorted(d.label(prm) for d, prm in roofs.values())))
        print("            segs    " + "  ".join(
            "%s %.1f..%.1f" % (sg["name"], sg["y0"], sg["y1"]) for sg in segs))
        print("            submesh " + "  ".join(
            "[%d]%s=%d" % (mi, nm, len(v)) for mi, nm, (v, _) in parts))

    print("")
    print("-> %s" % OUT_DIR)
    print('   models.json: "vertex": "models/buildings/<name>_v.bin", '
          '"index": "models/buildings/<name>_i.bin"')


if __name__ == "__main__":
    main()
