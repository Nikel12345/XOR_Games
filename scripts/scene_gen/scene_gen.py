#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
scene_gen.py — генератор игровой сцены для SDL_Engine (НОВЫЙ формат сцены-папки).

Сцена — ПАПКА: scene.sheaf (объекты) + рядом json-манифесты ресурсов
(materials.json / textures.json / models.json / shaders.json). Этот скрипт пишет
ТОЛЬКО scene.sheaf — сами объекты. Материалы/текстуры/модели/шейдеры уже приходят
из инита игры и остальных манифестов папки — скрипт лишь ссылается на них по имени.

scene.sheaf — бинарный формат Sheaf (docs/sheaf.md движка): таблица на архетип, колонка на поле
компонента, имена ассетов — строками, которые файл хранит по одному разу. Пишет его писатель
движка src/sheaf/sheaf.py. Таблицы идут по ключу архетипа (имена компонентов по алфавиту через
запятую), компоненты внутри — тоже по алфавиту: так пишет SaveScene движка, иначе пересохранение
сцены из редактора переставит таблицы. Все кубы делят ОДИН архетип → одна таблица.

Сцена набирается из СЕКЦИЙ (SECTIONS). Секция — ШАРОВОЙ СЛОЙ: кубы сыплются туда, где
inner_radius <= |(x,y,z)| <= outer_radius, но слой развёрнут по меридиану не целиком, а до
широты fill * 90 градусов. Отсюда форма: fill = 0 даёт плоский диск с вырезом (как было),
fill = 1 — законченный шар с шаровым же вырезом в центре, промежуточные значения — диск,
у которого с уходом по Y оба радиуса, внешний и внутренний, сжимаются по меридиану.
Поэтому |y| никогда не превысит outer_radius, а сам fill — величина безразмерная.
Разные секции = разные радиусы и своя завершённость, всё остальное общее. Пишутся они в
одну таблицу scene.sheaf: у всех секций одинаковый состав компонентов.

Ось Y — полноценная: орбитальная скорость считается по ПОЛНОМУ радиусу |(x,y,z)|, а не по
проекции на XZ, поэтому куб с ненулевой высотой летит по наклонной круговой орбите, а не
по кольцу на своей высоте. Требует ОБЪЁМНОЙ гравитации в GravitySystem.cpp::SimulateGravity
(ускорение по всем трём осям) — плоская XZ-гравитация такие орбиты порвёт.

Каждому кубу случайно назначается материал (из CUBE_MATERIALS) и модель
"cube_0".."cube_(N-1)". Модели — процедурные параллелепипеды из GravityScene.cpp
(цикл по kCubeVariants). NUM_CUBE_MODELS ниже ОБЯЗАН совпадать с kCubeVariants.

Запуск (без параметров):
    python scene_gen.py      (или: py scene_gen.py)

Результат пишется СРАЗУ в папку сцены игры: game/saved_scene/scene1M/scene.sheaf
(движок грузит папку "saved_scene/<имя сцены>" из рабочей папки game, см. Game::MainInit →
ctx->LoadScene("scene1")). Прочие манифесты в папке не трогаются.
"""

import os
import math
import random
import sys
from collections import namedtuple

# Писатель формата лежит в движке (src/sheaf/sheaf.py), а движок — каталог над games, как
# XOR_ENGINE_DIR в games/CMakeLists.txt.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "src", "sheaf"))
import sheaf  # noqa: E402

# ============================================================================
#  ПАРАМЕТРЫ ГЕНЕРАЦИИ  (правь здесь)
# ============================================================================
# --- Секции ---
# Каждая секция — свой объёмный диск (кольцо с вырезом), заполняется одной и той же логикой.
#   name          подпись в отчёте (в файл не идёт)
#   count         сколько кубов насыпать
#   inner_radius  РАДИУС ВЫРЕЗА: ближе к центру кубов нет (сферический вырез, не цилиндр)
#   outer_radius  МАКСИМАЛЬНЫЙ радиус
#   fill          завершённость шара, 0..1: доля меридиана, на которую развёрнут слой.
#                 0 = плоский диск, 0.5 = до широты 45 град., 1 = замкнутый шаровой слой.
#                 Величина безразмерная: высоту задаёт outer_radius (|y| <= outer_radius).
Section = namedtuple("Section", "name count inner_radius outer_radius fill")

SECTIONS = [
    #        имя           кубов   R внутр  R внеш   fill
    Section("inner_disk",  200000,    70.0,   180.0,   0.15),
    Section("mid_ring",    400000,   200.0,   380.0,   0.05),
    Section("outer_ring",  400000,   320.0,   550.0,   0.0),

]

# Общий множитель числа кубов во ВСЕХ секциях: единственная ручка, чтобы прогнать сцену в
# уменьшенном масштабе (0.1 -> 100k вместо 1M), не трогая пропорции между секциями.
COUNT_SCALE = 1.0

# --- Масштаб кубов ---
CUBE_SCALE_MIN = 0.3
CUBE_SCALE_MAX = 0.9

# --- Орбитальные скорости (круговая орбита вокруг центра сцены (0,0,0)) ---
# Предполагаем в центре гравитационный объект. Скорость круговой орбиты: v = sqrt(G*M / r),
# где r — ПОЛНОЕ расстояние до центра, |(x,y,z)|. Настоящая G = 6.674e-11 не нужна: её степень «съедена»
# массой (работаем сразу с произведением GM), иначе для нормальных скоростей масса была бы
# астрономической. Крути CENTRAL_MASS (или GRAVITY_CONST), чтобы менять темп вращения.
# GM ОБЯЗАН совпадать с полем gm у сущности-центра (GravityComponent), иначе орбиты «поедут»:
# скорости здесь считаются по нему, а притягивает в игре именно оно.
GRAVITY_CONST = 1.0         # G без крошечной степени
CENTRAL_MASS = 5000.0       # масса гравитационного объекта в (0,0)
GM = GRAVITY_CONST * CENTRAL_MASS   # μ — стандартный гравитационный параметр
ORBIT_SPEED_SPREAD = 0.07   # индивидуальный разброс скорости, доля (±доля); 0 = идеальные круги
SIM_DT = 0.05

# --- Сущность-центр притяжения (Transform + GravityComponent) ---
# Гравитация в игре привязана к СУЩНОСТИ: центр там, где её Transform, сила = её gm. Поэтому
# центр генерируется сюда же, в сцену, — без него кубы полетят по инерции. Renderable
# ему нужен только чтобы его было видно в кадре и в списке объектов редактора.
GRAVITY_CENTER_POS = (0.0, 0.0, 0.0)
GRAVITY_CENTER_SCALE = 12.0
GRAVITY_CENTER_MODEL = "cube_0"
GRAVITY_CENTER_MATERIAL = "emission"

# Фиксированный сид → одна и та же сцена при каждом запуске (None = каждый раз новая).
RANDOM_SEED = 42

# Материалы (уже зарегистрированы в Game.cpp / materials.json) — раздаются кубам случайно.
CUBE_MATERIALS = ["m_orange", "m_gray", "metal1", "metal2", "emission"]
# Уровни моделей cube_* в игре (GravityScene.cpp): 1 — quad, 2 — точка. Материал уровня — тот же с этим
# суффиксом, на программе LOD_Quad / LOD_Splat (materials.json сцены).
CUBE_LEVEL_SUFFIXES = ["", "_lod", "_splat"]

# ----------------------------------------------------------------------------
#  Модели кубов — процедурные параллелепипеды из GravityScene.cpp с именами cube_0..cube_(N-1).
#  Питон только раздаёт эти имена в поле Renderable.models сцены; сама геометрия строится в игре.
#  NUM_CUBE_MODELS ДОЛЖЕН быть равен kCubeVariants в GravityScene.cpp — иначе имена не сойдутся
#  (движок не найдёт модель по имени и сущность не отрисуется).
# ----------------------------------------------------------------------------
NUM_CUBE_MODELS = 12
CUBE_MODELS = ["cube_{}".format(i) for i in range(NUM_CUBE_MODELS)]

# --- Направленный свет сцены (entity 0) ---
# ВЫКЛЮЧЕН (2026-08-02): солнце с ShadowCaster на этой сцене — главный пожиратель кадра
# (light-группа каллинга гоняет 1М строк × каскады + отрисовка 1М кастеров в 3 каскада
# КАЖДЫЙ кадр, от видимости player-камерой не зависит → 15 FPS вместо 60). Свет, если
# нужен, заводить отдельно и с разумным half_extent, а не на весь диск.
EMIT_DIRECT_LIGHT = False
DIRECT_LIGHT_DIR = (0.0, -1.0, -0.7)
DIRECT_LIGHT_COLOR = (1.0, 1.0, 1.0)
DIRECT_LIGHT_POWER = 2.5
LIGHT_CASCADE_COUNT = 3
LIGHT_CASCADE_RATIO = 3.0

# Папка сцены игры (относительно скрипта) и имя ECS-файла внутри неё. Сцена — это
# подпапка корня сцен по её имени (saved_scene/<имя>), а не сам saved_scene.
SCENE_DIR = os.path.join("..", "..", "game", "saved_scene", "scene1M")
OUTPUT_NAME = "scene.sheaf"

# Имена 16 колонок Transform (row-major 4x4; трансляция в w/d/h — индексы 3/7/11).
# Порядок = порядок полей в ComponentSerializer.cpp (реестр Transform).
TRANSFORM_COLS = ["x", "y", "z", "w", "a", "b", "c", "d",
                  "e", "f", "g", "h", "i", "j", "k", "l"]


# ============================================================================
#  Математика матриц (движок ждёт 4x4 row-major, трансляция в индексах 3/7/11)
# ============================================================================
def _rot_x(t):
    c, s = math.cos(t), math.sin(t)
    return [[1, 0, 0], [0, c, -s], [0, s, c]]


def _rot_y(t):
    c, s = math.cos(t), math.sin(t)
    return [[c, 0, s], [0, 1, 0], [-s, 0, c]]


def _rot_z(t):
    c, s = math.cos(t), math.sin(t)
    return [[c, -s, 0], [s, c, 0], [0, 0, 1]]


def _mul3(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def make_transform(pos, rot3x3, scale):
    """Собирает 16 float (row-major) из позиции, поворота 3x3 и равномерного масштаба."""
    x, y, z = pos
    m = [[rot3x3[r][c] * scale for c in range(3)] for r in range(3)]
    return [
        m[0][0], m[0][1], m[0][2], x,
        m[1][0], m[1][1], m[1][2], y,
        m[2][0], m[2][1], m[2][2], z,
        0.0,     0.0,     0.0,     1.0,
    ]


# ============================================================================
#  Генерация точек / поворотов / скоростей
# ============================================================================
def sample_point(inner_radius, outer_radius, fill):
    """Случайная точка шарового слоя секции, развёрнутого по меридиану на fill * 90 градусов.

    Радиус |(x,y,z)| берётся по sqrt — равномерно по площади кольца, ровно как раньше: правится
    ТОЛЬКО раскладка по широте. Широта задаётся через СИНУС, а не через сам угол: у сферы
    площадь пояса пропорциональна перепаду синуса широты, поэтому равномерный синус даёт
    равномерную плотность, а равномерный угол сбил бы кубы к полюсам.

    При fill = 0 синус всегда 0 → y = 0 и точка ложится на прежнее плоское кольцо; при fill = 1
    синус равномерен на [-1, 1] → полный шаровой слой. И там и там |y| <= |(x,y,z)| <= outer.
    """
    u = random.random()
    rho = math.sqrt(u * (outer_radius ** 2 - inner_radius ** 2) + inner_radius ** 2)
    sin_lat = random.uniform(-1.0, 1.0) * math.sin(fill * math.pi * 0.5)
    r_xz = rho * math.sqrt(max(0.0, 1.0 - sin_lat * sin_lat))
    theta = random.uniform(0.0, 2.0 * math.pi)
    return (r_xz * math.cos(theta), rho * sin_lat, r_xz * math.sin(theta))


def random_rotation():
    """Полностью случайная ориентация (Rz * Ry * Rx) — кубы «плавают» в объёме."""
    rx = _rot_x(random.uniform(0.0, 2.0 * math.pi))
    ry = _rot_y(random.uniform(0.0, 2.0 * math.pi))
    rz = _rot_z(random.uniform(0.0, 2.0 * math.pi))
    return _mul3(_mul3(rz, ry), rx)


def orbital_velocity(pos):
    """Вектор скорости для круговой орбиты вокруг центра (0,0,0). Правило то же, что было
    в XZ, только радиус теперь ПОЛНЫЙ: |v| = sqrt(GM / |(x,y,z)|), направление —
    перпендикуляр к радиусу, разброс ±ORBIT_SPEED_SPREAD.

    Перпендикуляров к радиусу бесконечно много; берём тот, что даёт общий для всей сцены обход
    вокруг оси Y. Тогда куб с ненулевой высотой летит по НАКЛОННОЙ круговой орбите (её
    плоскость проходит через центр и через сам куб), а не по кольцу на своей высоте. Поэтому
    vy стартует нулевым: его набирает сама гравитация, когда куб уходит по орбите вниз.

    Считать модуль по XZ-радиусу, как раньше, при заметной высоте секции уже нельзя: r_xz < r,
    скорость вышла бы завышенной и круговая орбита раскрутилась бы в эллипс.
    """
    x, y, z = pos
    r = math.sqrt(x * x + y * y + z * z)
    r_xz = math.hypot(x, z)
    if r < 1e-6 or r_xz < 1e-6:
        return (0.0, 0.0, 0.0)
    v = math.sqrt(GM / r)
    v *= 1.0 + random.uniform(-ORBIT_SPEED_SPREAD, ORBIT_SPEED_SPREAD)
    # (-z, 0, x)/r_xz — единичный и строго перпендикулярный (x,y,z) при ЛЮБОМ y:
    # скалярное произведение = (-z*x + x*z)/r_xz = 0.
    return (-z / r_xz * v, 0.0, x / r_xz * v)


# ============================================================================
#  Сериализация scene.sheaf
# ============================================================================
MAX_LOD = 4   # колонки mat_lod0..mat_lod3 (MAX_LOD в BaseComponents.h)

# Дефолт Transform — единичная матрица, как у PositionProxy16 в BaseComponents.h.
TRANSFORM_DEFAULT = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]


def _add_transform(t, columns):
    c = t.component("Transform")
    for k in range(16):
        c.field(TRANSFORM_COLS[k], sheaf.F32, columns[k], default=TRANSFORM_DEFAULT[k])


def _add_renderable(t, models, lods):
    """Renderable в той раскладке, что пишет движок (MakeSaveRenderable в Engine.cpp): у объекта
    одна часть, lods[i] — её материалы по уровням."""
    n = len(models)
    c = t.component("Renderable")
    c.field("visible", sheaf.BOOL, [True] * n, default=True)
    c.field("alpha", sheaf.F32, [1.0] * n, default=1.0)
    c.field("flags", sheaf.U32, [0] * n)
    c.field("model", sheaf.STR, models)
    for L in range(MAX_LOD):
        col = [[row[L] if L < len(row) else None] for row in lods]
        # mat_lod0 — всегда: по длинам его списков движок узнаёт число частей объекта.
        if L == 0 or any(cell[0] is not None for cell in col):
            c.field("mat_lod%d" % L, sheaf.STR, col, flags=sheaf.LIST | sheaf.NULLABLE)


def _add_velocity(t, vx, vy, vz):
    c = t.component("Velocity")
    c.field("x", sheaf.F32, vx)
    c.field("y", sheaf.F32, vy)
    c.field("z", sheaf.F32, vz)


def _single(w, *components):
    """Таблица из одного объекта. components — пары (имя, [(поле, тип, значение), ...]) или
    готовые функции t -> None (Renderable/Transform), по алфавиту имён."""
    t = w.table(1)
    for comp in components:
        if callable(comp):
            comp(t)
            continue
        name, fields = comp
        c = t.component(name)
        for field, type_, value in fields:
            c.field(field, type_, [value])
    return t


def _light_table(w):
    """DirectLight,ShadowCaster: один объект."""
    dx, dy, dz = DIRECT_LIGHT_DIR
    r, g, b = DIRECT_LIGHT_COLOR
    he = max(sec.outer_radius for sec in SECTIONS)   # шаровой слой целиком влезает в этот радиус
    F = sheaf.F32
    _single(w, ("DirectLight", [
        ("dir_x", F, dx), ("dir_y", F, dy), ("dir_z", F, dz),
        ("r", F, r), ("g", F, g), ("b", F, b),
        ("power", F, DIRECT_LIGHT_POWER),
        ("center_x", F, 0.0), ("center_y", F, 0.0), ("center_z", F, 0.0),
        ("half_extent", F, he), ("half_depth", F, he),
        ("cascade_count", sheaf.U32, int(LIGHT_CASCADE_COUNT)),
        ("cascade_ratio", F, LIGHT_CASCADE_RATIO),
    ]), ("ShadowCaster", []))


def _center_transform():
    ident = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]
    m = make_transform(GRAVITY_CENTER_POS, ident, GRAVITY_CENTER_SCALE)
    return lambda t: _add_transform(t, [[v] for v in m])


def _center_renderable():
    return lambda t: _add_renderable(t, [GRAVITY_CENTER_MODEL], [[GRAVITY_CENTER_MATERIAL]])


class _Columns:
    # Колонки одного архетипа (SoA): значение i-го куба лежит в i-й позиции каждой колонки.
    # Секции — не отдельные таблицы, а порции строк одной: состав компонентов у них одинаковый.

    def __init__(self):
        self.transform = [[] for _ in range(16)]
        self.model = []
        self.lods = []         # материалы единственной части куба по уровням
        self.vx, self.vy, self.vz = [], [], []
        self.jet_center = []   # только у колонок джетов: Jet.center

    def count(self):
        return len(self.model)


def emit_section(section, cols):
    """Досыпает кубы одной секции в общие колонки cols.

    Единственное, что меняется от секции к секции, — радиусы и высота области; строки всех
    секций идут подряд в одну таблицу.
    """
    for _ in range(section.count):
        pos = sample_point(section.inner_radius, section.outer_radius, section.fill)
        rot = random_rotation()
        scale = random.uniform(CUBE_SCALE_MIN, CUBE_SCALE_MAX)
        transform = make_transform(pos, rot, scale)
        for k in range(16):
            cols.transform[k].append(transform[k])

        cols.model.append(random.choice(CUBE_MODELS))
        mat = random.choice(CUBE_MATERIALS)
        cols.lods.append([mat + s for s in CUBE_LEVEL_SUFFIXES])

        vx, vy, vz = orbital_velocity(pos)
        cols.vx.append(vx)
        cols.vy.append(vy)
        cols.vz.append(vz)


# Ключи архетипов = отсортированные по алфавиту имена компонентов через запятую (так их строит
# SaveScene движка). По ним же определяется порядок таблиц в файле.
LIGHT_ARCHETYPE = "DirectLight,ShadowCaster"
CUBES_ARCHETYPE = "Renderable,Shadow,Transform,Velocity"
CENTER_ARCHETYPE = "Gravity,Renderable,Transform"
GRAVITY_WORLD_ARCHETYPE = "GravityWorld"


def _cubes_table(w, cols):
    t = w.table(cols.count())
    _add_renderable(t, cols.model, cols.lods)
    t.component("Shadow")
    _add_transform(t, cols.transform)
    _add_velocity(t, cols.vx, cols.vy, cols.vz)


def _center_table(w):
    """Сама сущность-центр (одна штука)."""
    _single(w, ("Gravity", [("gm", sheaf.F32, GM)]), _center_renderable(), _center_transform())


def _gravity_world_table(w):
    _single(w, ("GravityWorld", [("sim_dt", sheaf.F32, SIM_DT)]))


def resolved_sections():
    """SECTIONS с применённым COUNT_SCALE и проверкой параметров; пустые секции отброшены."""
    out = []
    for sec in SECTIONS:
        if sec.inner_radius < 0 or sec.outer_radius <= sec.inner_radius:
            raise ValueError("Секция '{}': нужно 0 <= inner_radius < outer_radius".format(sec.name))
        if not (0.0 <= sec.fill <= 1.0):
            raise ValueError("Секция '{}': fill должен лежать в 0..1".format(sec.name))
        n = int(round(sec.count * COUNT_SCALE))
        if n > 0:
            out.append(sec._replace(count=n))
    if not out:
        raise ValueError("Ни одной непустой секции (проверь COUNT_SCALE)")
    return out


def build_scene():
    sections = resolved_sections()

    if RANDOM_SEED is not None:
        random.seed(RANDOM_SEED)

    cols = _Columns()
    for sec in sections:
        emit_section(sec, cols)

    w = sheaf.Writer()
    tables = {CENTER_ARCHETYPE: lambda: _center_table(w), CUBES_ARCHETYPE: lambda: _cubes_table(w, cols),
              GRAVITY_WORLD_ARCHETYPE: lambda: _gravity_world_table(w)}
    if EMIT_DIRECT_LIGHT:
        tables[LIGHT_ARCHETYPE] = lambda: _light_table(w)
    for key in sorted(tables):
        tables[key]()
    return w.finish(), set(cols.model), sections


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    out_dir = os.path.join(script_dir, SCENE_DIR)
    out_path = os.path.join(out_dir, OUTPUT_NAME)

    if not os.path.isdir(out_dir):
        raise SystemExit("Папки сцены нет: {}\n(ожидается game/saved_scene/scene1M с манифестами ресурсов)".format(out_dir))

    data, used_models, sections = build_scene()

    with open(out_path, "wb") as f:
        f.write(data)

    total = sum(sec.count for sec in sections)
    print("OK: {} кубов в {} секц. -> {}".format(total, len(sections), out_path))
    for sec in sections:
        print("  {:<12} {:>9} кубов   R {:g}..{:g}   fill {:g} (широта +/-{:.1f} град., |y| до {:.1f})".format(
            sec.name, sec.count, sec.inner_radius, sec.outer_radius, sec.fill,
            sec.fill * 90.0, sec.outer_radius * math.sin(sec.fill * math.pi * 0.5)))
    if COUNT_SCALE != 1.0:
        print("  (COUNT_SCALE={:g})".format(COUNT_SCALE))
    print("Свет: {}".format("да" if EMIT_DIRECT_LIGHT else "нет"))
    print("Использовано моделей: {} из {} (cube_0..cube_{}).".format(
        len(used_models), NUM_CUBE_MODELS, NUM_CUBE_MODELS - 1))
    print("Проверь: kCubeVariants в GravityScene.cpp == NUM_CUBE_MODELS ({}), "
          "kGravGM == GM ({}).".format(NUM_CUBE_MODELS, GM))


if __name__ == "__main__":
    main()
