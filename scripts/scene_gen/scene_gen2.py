#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
scene_gen2.py — генератор игровой сцены для SDL_Engine (НОВЫЙ формат сцены-папки).

Копия scene_gen.py, к которой добавлены СПИРАЛЬНЫЕ РУКАВА и ДЖЕТЫ.

Рукава: кубы секций ложатся не равномерно по кругу, а вдоль N логарифмических спиралей (SPIRAL_*).
Чтобы рукава не закрутились дифференциальным вращением, центр — однородный шар радиуса
GRAVITY_CORE_RADIUS: внутри него притяжение линейно по расстоянию, период обращения одинаков на
любом радиусе, и узор вращается целиком (см. GRAVITY_CORE_RADIUS).

Джеты: у каждого центра гравитации — пул кубов,
летящих из него двумя встречными конусами вдоль его оси Y (см. JET_* ниже). Кубы джета
несут компонент Jet со ссылкой на свой центр (Jet.center == Gravity.id); гравитация их не
тянет, а ушедший дальше JET_RANGE куб игра возвращает в его центр с той же скоростью
(GravitySystem.cpp::ReturnJets).

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
    python scene_gen2.py      (или: py scene_gen2.py)

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
    Section("inner_disk",  200000,    40.0,   220.0,   0.04),
    Section("mid_ring",    400000,   220.0,   420.0,   0.025),
    Section("outer_ring",  400000,   370.0,   650.0,   0.0),

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
CENTRAL_MASS = 200000.0     # масса гравитационного объекта в (0,0); x40 к scene_gen.py — оборот диска ~1 мин
GM = GRAVITY_CONST * CENTRAL_MASS   # μ — стандартный гравитационный параметр
ORBIT_SPEED_SPREAD = 0.07   # индивидуальный разброс скорости, доля (±доля); 0 = идеальные круги

# Радиус однородного шара-центра (пишется в Gravity.core_radius). Внутри шара ускорение
# a = GM*r/R^3 — линейно по r, поэтому угловая скорость omega = sqrt(GM/R^3) у всех кубов одна:
# диск вращается как колесо и рукава не закручиваются. Снаружи — обычное GM/r^2. Весь диск
# должен лежать внутри шара, поэтому по умолчанию R = внешний радиус самой дальней секции.
# 0 = точечная масса, как в scene_gen.py (рукава закрутятся за пару минут).
# Период оборота 2*pi*sqrt(R^3/GM): быстрее вращение — больше CENTRAL_MASS (период ~ 1/sqrt(GM)).
GRAVITY_CORE_RADIUS = max(sec.outer_radius for sec in SECTIONS)
SIM_DT = 0.05

# --- Спиральные рукава ---
# Рукав — логарифмическая спираль theta = theta_k - ln(r / r0) / tan(pitch) в плоскости XZ.
# Знак минус — рукава ЗАКРУЧЕНЫ ПРОТИВ вращения (трейлинговые, как у настоящих галактик):
# концы рукавов отстают от вращения. Радиус и высоту точки по-прежнему дают секции.
SPIRAL_ARMS = 4             # число рукавов
SPIRAL_PITCH_DEG = 15.0     # угол закрутки: между рукавом и окружностью; меньше — туже спираль
SPIRAL_ARM_WIDTH = 75.0     # ширина рукава (sigma поперёк, юниты мира) — одна на любом радиусе
SPIRAL_BACKGROUND = 0.1     # доля кубов вне рукавов, равномерно по кругу

# --- Сущность-центр притяжения (Transform + GravityComponent) ---
# Гравитация в игре привязана к СУЩНОСТИ: центр там, где её Transform, сила = её gm. Поэтому
# центр генерируется сюда же, в сцену, — без него кубы полетят по инерции. Renderable
# ему нужен только чтобы его было видно в кадре и в списке объектов редактора.
GRAVITY_CENTER_POS = (0.0, 0.0, 0.0)
# Центр — чёрная сфера: движковая модель "sphere" радиуса 1, масштаб = радиус в юнитах мира.
# Материал black_hole (materials.json сцены) — metallic 1 при нулевом цвете: гаснут и диффуз,
# и отражение окружения.
GRAVITY_CENTER_SCALE = 25.0
GRAVITY_CENTER_MODEL = "sphere"
GRAVITY_CENTER_MATERIAL = "black_hole"
GRAVITY_CENTER_SCHWARZSCHILD_RADIUS = GRAVITY_CENTER_SCALE / (1.5 * math.sqrt(3.0))

# --- Скайбокс ---
# Сущность из одного Renderable, без Transform: вершинник скайбокса ставит куб вокруг камеры сам.
# Модель, материал и env-кубмапа (env_skybox_cube) — в манифестах сцены.
SKYBOX_MODEL = "skybox_cube"
SKYBOX_MATERIAL = "skybox"

# --- Джеты (на КАЖДЫЙ центр гравитации) ---
# Два встречных конуса вдоль оси Y центра, кубы делятся между ними поровну. Скорость у всех
# одна и та же по модулю, направление — случайное внутри конуса.
JET_COUNT = 35000         # кубов на центр, на оба конуса вместе
JET_SPREAD_DEG = 3.0      # полуугол конуса, градусы
JET_SPEED = 30.0          # юниты/с (в тех же единицах, что и орбитальные скорости)
# Дистанция от центра, дальше которой игра возвращает куб в центр. Кубы изначально разложены по
# всей длине джета: если бы все стартовали из центра, то при одной скорости они и возвращались
# бы разом — джет пульсировал бы волной вместо ровной струи.
JET_RANGE = 2500.0
# Материал уровней LOD — через те же суффиксы CUBE_LEVEL_SUFFIXES (jet / jet_lod / jet_splat).
JET_MATERIAL = "jet"

# Фиксированный сид → одна и та же сцена при каждом запуске (None = каждый раз новая).
RANDOM_SEED = 42

# Материалы (materials.json сцены) — раздаются кубам случайно.
CUBE_MATERIALS = ["Emission_LitColor"]
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
    """Случайная точка шарового слоя секции, развёрнутого по меридиану на fill * 90 градусов;
    по кругу — вдоль спиральных рукавов (spiral_theta).

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
    theta = spiral_theta(r_xz)
    return (r_xz * math.cos(theta), rho * sin_lat, r_xz * math.sin(theta))


# Радиус, на котором рукава начинаются с углов 2*pi*k/N, — внутренняя кромка диска. Сдвигает
# только поворот узора целиком.
_SPIRAL_R0 = max(1.0, min(sec.inner_radius for sec in SECTIONS))


def spiral_theta(r_xz):
    """Угол точки на радиусе r_xz: SPIRAL_BACKGROUND из них — равномерно по кругу, остальные —
    на случайном рукаве с гауссовым разбросом поперёк. Разброс задан в юнитах и переведён в угол
    делением на радиус, поэтому рукав одной ширины и у центра, и на краю."""
    if random.random() < SPIRAL_BACKGROUND or r_xz < 1e-6:
        return random.uniform(0.0, 2.0 * math.pi)
    arm = random.randrange(SPIRAL_ARMS)
    wind = math.log(r_xz / _SPIRAL_R0) / math.tan(math.radians(SPIRAL_PITCH_DEG))
    return 2.0 * math.pi * arm / SPIRAL_ARMS - wind + random.gauss(0.0, SPIRAL_ARM_WIDTH / r_xz)


def random_rotation():
    """Полностью случайная ориентация (Rz * Ry * Rx) — кубы «плавают» в объёме."""
    rx = _rot_x(random.uniform(0.0, 2.0 * math.pi))
    ry = _rot_y(random.uniform(0.0, 2.0 * math.pi))
    rz = _rot_z(random.uniform(0.0, 2.0 * math.pi))
    return _mul3(_mul3(rz, ry), rx)


def orbital_velocity(pos):
    """Вектор скорости для круговой орбиты вокруг центра (0,0,0), разброс ±ORBIT_SPEED_SPREAD.

    Внутри шара-центра (|p| < GRAVITY_CORE_RADIUS) — вращение колесом: v = omega * (-z, 0, x),
    омега общая на всех. Тогда в проекции на XZ узор поворачивается как целое и рукава держат
    форму. Высота y при этом сама качается с тем же периодом (притяжение шара тянет и по y):
    толщина диска «дышит», а вид сверху не меняется.

    Снаружи шара — прежнее правило ниже.

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
    spread = 1.0 + random.uniform(-ORBIT_SPEED_SPREAD, ORBIT_SPEED_SPREAD)
    if r < GRAVITY_CORE_RADIUS:
        w = math.sqrt(GM / GRAVITY_CORE_RADIUS ** 3) * spread
        return (-z * w, 0.0, x * w)
    v = math.sqrt(GM / r) * spread
    # (-z, 0, x)/r_xz — единичный и строго перпендикулярный (x,y,z) при ЛЮБОМ y:
    # скалярное произведение = (-z*x + x*z)/r_xz = 0.
    return (-z / r_xz * v, 0.0, x / r_xz * v)


def jet_direction(axis_sign):
    """Случайное единичное направление внутри конуса с полууглом JET_SPREAD_DEG вокруг
    (0, axis_sign, 0). Косинус угла от оси берётся равномерно — так точки равномерны по
    площади сферической шапки, а не сгущены к оси."""
    cos_max = math.cos(math.radians(JET_SPREAD_DEG))
    cos_t = random.uniform(cos_max, 1.0)
    sin_t = math.sqrt(max(0.0, 1.0 - cos_t * cos_t))
    phi = random.uniform(0.0, 2.0 * math.pi)
    return (sin_t * math.cos(phi), axis_sign * cos_t, sin_t * math.sin(phi))


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


def emit_jets(center_pos, center_id, cols):
    """Досыпает кубы двух джетов одного центра в колонки cols (свои, не колонки секций: у
    кубов джета другой архетип). Конусы чередуются через куб — делятся поровну. center_id
    уходит в Jet.center — по нему игра находит центр куба."""
    cx, cy, cz = center_pos
    lods = [JET_MATERIAL + s for s in CUBE_LEVEL_SUFFIXES]
    for i in range(JET_COUNT):
        dx, dy, dz = jet_direction(1.0 if i % 2 == 0 else -1.0)
        t = random.uniform(0.0, JET_RANGE)
        pos = (cx + dx * t, cy + dy * t, cz + dz * t)

        scale = random.uniform(CUBE_SCALE_MIN, CUBE_SCALE_MAX)
        transform = make_transform(pos, random_rotation(), scale)
        for k in range(16):
            cols.transform[k].append(transform[k])

        cols.model.append(random.choice(CUBE_MODELS))
        cols.lods.append(lods)

        cols.vx.append(dx * JET_SPEED)
        cols.vy.append(dy * JET_SPEED)
        cols.vz.append(dz * JET_SPEED)
        cols.jet_center.append(center_id)


# Ключи архетипов = отсортированные по алфавиту имена компонентов через запятую (так их строит
# SaveScene движка). По ним же определяется порядок таблиц в файле.
LIGHT_ARCHETYPE = "DirectLight,ShadowCaster"
CUBES_ARCHETYPE = "Renderable,Shadow,Transform,Velocity"
CENTER_ARCHETYPE = "GravitationalLens,Gravity,Renderable,Transform"
JETS_ARCHETYPE = "Jet,Renderable,Transform,Velocity"
SKYBOX_ARCHETYPE = "Renderable"
GRAVITY_WORLD_ARCHETYPE = "GravityWorld"


def _cubes_table(w, cols):
    t = w.table(cols.count())
    _add_renderable(t, cols.model, cols.lods)
    t.component("Shadow")
    _add_transform(t, cols.transform)
    _add_velocity(t, cols.vx, cols.vy, cols.vz)


def _jets_table(w, cols):
    t = w.table(cols.count())
    t.component("Jet").field("center", sheaf.U32, cols.jet_center)
    _add_renderable(t, cols.model, cols.lods)
    _add_transform(t, cols.transform)
    _add_velocity(t, cols.vx, cols.vy, cols.vz)


def _center_table(w, center_id):
    """Сама сущность-центр (одна штука)."""
    F = sheaf.F32
    _single(w,
            ("GravitationalLens", [("schwarzschild_radius", F, GRAVITY_CENTER_SCHWARZSCHILD_RADIUS),
                                   ("inner_radius", F, GRAVITY_CENTER_SCALE)]),
            ("Gravity", [("gm", F, GM), ("id", sheaf.U32, center_id), ("core_radius", F, GRAVITY_CORE_RADIUS)]),
            _center_renderable(), _center_transform())


def _skybox_table(w):
    _single(w, lambda t: _add_renderable(t, [SKYBOX_MODEL], [[SKYBOX_MATERIAL]]))


def _gravity_world_table(w):
    _single(w, ("GravityWorld", [("sim_dt", sheaf.F32, SIM_DT), ("jet_return_distance", sheaf.F32, JET_RANGE)]))


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

    # Джеты — ПОСЛЕ секций: тогда правка JET_* не сдвигает случайную последовательность секций
    # и раскладка диска от неё не меняется.
    jet_cols = _Columns()
    # id центра = его номер в списке: у каждого центра свой, по нему джеты ссылаются на центр.
    centers = [GRAVITY_CENTER_POS]
    for center_id, center_pos in enumerate(centers):
        emit_jets(center_pos, center_id, jet_cols)
    n_jets = jet_cols.count()

    w = sheaf.Writer()
    tables = {CENTER_ARCHETYPE: lambda: _center_table(w, 0),   # единственный центр = centers[0]
              CUBES_ARCHETYPE: lambda: _cubes_table(w, cols),
              SKYBOX_ARCHETYPE: lambda: _skybox_table(w),
              GRAVITY_WORLD_ARCHETYPE: lambda: _gravity_world_table(w)}
    if n_jets:
        tables[JETS_ARCHETYPE] = lambda: _jets_table(w, jet_cols)
    if EMIT_DIRECT_LIGHT:
        tables[LIGHT_ARCHETYPE] = lambda: _light_table(w)
    for key in sorted(tables):
        tables[key]()
    return w.finish(), set(cols.model), sections, n_jets


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    out_dir = os.path.join(script_dir, SCENE_DIR)
    out_path = os.path.join(out_dir, OUTPUT_NAME)

    if not os.path.isdir(out_dir):
        raise SystemExit("Папки сцены нет: {}\n(ожидается game/saved_scene/scene1M с манифестами ресурсов)".format(out_dir))

    data, used_models, sections, n_jets = build_scene()

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
    if GRAVITY_CORE_RADIUS > 0:
        period = 2.0 * math.pi * math.sqrt(GRAVITY_CORE_RADIUS ** 3 / GM)
        print("Рукава: {}, закрутка {:g} град., ширина {:g}; шар-центр R {:g}, оборот диска {:.0f} тиков".format(
            SPIRAL_ARMS, SPIRAL_PITCH_DEG, SPIRAL_ARM_WIDTH, GRAVITY_CORE_RADIUS, period / SIM_DT))
    print("Джеты: {} кубов ({} на центр), конус +/-{:g} град., скорость {:g}, возврат дальше {:g}".format(
        n_jets, JET_COUNT, JET_SPREAD_DEG, JET_SPEED, JET_RANGE))
    print("Свет: {}".format("да" if EMIT_DIRECT_LIGHT else "нет"))
    print("Использовано моделей: {} из {} (cube_0..cube_{}).".format(
        len(used_models), NUM_CUBE_MODELS, NUM_CUBE_MODELS - 1))
    print("Проверь: kCubeVariants в GravityScene.cpp == NUM_CUBE_MODELS ({}), "
          "kGravGM == GM ({}).".format(NUM_CUBE_MODELS, GM))


if __name__ == "__main__":
    main()
