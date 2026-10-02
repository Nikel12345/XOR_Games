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

Сцена теперь — ПАПКА: scene.json (ECS) + рядом манифесты ресурсов
(materials.json / textures.json / models.json / shaders.json). Этот скрипт пишет
ТОЛЬКО scene.json — сами объекты. Материалы/текстуры/модели/шейдеры уже приходят
из инита игры и остальных манифестов папки — скрипт лишь ссылается на них по имени.

Формат scene.json (см. ObjectManager::SaveScene / ComponentSpec::Save):
    { "materials": [имена], "models": [имена],
      "<архетип>": { "count": N, "entities": [...], "<Компонент>": { "<поле>": [колонка] } } }
Ключ архетипа = ОТСОРТИРОВАННЫЕ по алфавиту имена компонентов через запятую; в ТОМ ЖЕ
порядке идут блоки компонентов внутри — так пишет движок, иначе пересохранение сцены
из редактора переставит их и даст пустой дифф на весь файл.
Внутри каждый компонент — КОЛОНКИ по полям (SoA-стиль): значение i-й сущности лежит
в i-й позиции каждой колонки. Все кубы делят ОДИН архетип → один компактный блок.

Имена ассетов лежат в СЛОВАРЯХ в шапке файла ("models"/"materials"), а колонки
Renderable.models и Renderable.materials хранят ИНДЕКС в них: на миллионе кубов десяток имён иначе повторяется миллион
раз. Движок принимает в ячейке и строку («имя как есть»), но пишем индексами — ради этого
словарь и заводился (см. ScenePool в ComponentSerializer.h).

Сцена набирается из СЕКЦИЙ (SECTIONS). Секция — ШАРОВОЙ СЛОЙ: кубы сыплются туда, где
inner_radius <= |(x,y,z)| <= outer_radius, но слой развёрнут по меридиану не целиком, а до
широты fill * 90 градусов. Отсюда форма: fill = 0 даёт плоский диск с вырезом (как было),
fill = 1 — законченный шар с шаровым же вырезом в центре, промежуточные значения — диск,
у которого с уходом по Y оба радиуса, внешний и внутренний, сжимаются по меридиану.
Поэтому |y| никогда не превысит outer_radius, а сам fill — величина безразмерная.
Разные секции = разные радиусы и своя завершённость, всё остальное общее. Пишутся они в
ОДИН scene.json: у всех секций одинаковый состав компонентов, значит один блок архетипа,
сквозная нумерация сущностей и общие словари имён.

Ось Y — полноценная: орбитальная скорость считается по ПОЛНОМУ радиусу |(x,y,z)|, а не по
проекции на XZ, поэтому куб с ненулевой высотой летит по наклонной круговой орбите, а не
по кольцу на своей высоте. Требует ОБЪЁМНОЙ гравитации в GravitySystem.cpp::SimulateGravity
(ускорение по всем трём осям) — плоская XZ-гравитация такие орбиты порвёт.

Каждому кубу случайно назначается материал (из CUBE_MATERIALS) и модель
"cube_0".."cube_(N-1)". Модели — процедурные параллелепипеды из GravityScene.cpp
(цикл по kCubeVariants). NUM_CUBE_MODELS ниже ОБЯЗАН совпадать с kCubeVariants.

Запуск (без параметров):
    python scene_gen2.py      (или: py scene_gen2.py)

Результат пишется СРАЗУ в папку сцены игры: game/saved_scene/scene1M/scene.json
(движок грузит папку "saved_scene/<имя сцены>" из рабочей папки game, см. Game::MainInit →
ctx->LoadScene("scene1")). Прочие манифесты в папке не трогаются.
"""

import os
import math
import random
from collections import namedtuple

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
OUTPUT_NAME = "scene.json"

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


def _fmt(v):
    # %.7g — точность float32 (как SaveTransform движка) и компактный, но валидный JSON-номер.
    return format(v, ".7g")


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
#  Сериализация scene.json (архетип-колоночный формат)
# ============================================================================
def _num_col(key, values):
    """'"key":[v0,v1,...]' — числовая колонка из уже отформатированных строк."""
    return '"{}":[{}]'.format(key, ",".join(values))


class _Pool:
    # Словарь имён одного вида ассета — то же, что ScenePool::List в движке.
    # Индекс выдаётся по первому появлению имени, поэтому неиспользованных записей в списке
    # не будет: сколько имён реально роздано кубам, столько и попадёт в файл.

    def __init__(self):
        self.names = []
        self._index = {}

    def intern(self, name):
        i = self._index.get(name)
        if i is None:
            i = len(self.names)
            self._index[name] = i
            self.names.append(name)
        return i

    def json(self, key):
        return '"{}":[{}]'.format(key, ",".join('"{}"'.format(n) for n in self.names))


class _Columns:
    # Колонки сцены (SoA) и словари имён — ОБЩИЕ на все секции: секции не отдельные блоки
    # файла, а порции строк в одном блоке архетипа (состав компонентов у них одинаковый).

    def __init__(self):
        self.transform = [[] for _ in range(16)]
        self.model = []
        self.material = []
        self.vx, self.vy, self.vz = [], [], []
        self.jet_center = []   # только у колонок джетов: Jet.center
        self.models = _Pool()
        self.materials = _Pool()

    def count(self):
        return len(self.model)


def emit_section(section, cols):
    """Досыпает кубы одной секции в общие колонки cols.

    Это ровно прежняя логика генерации, вынесенная в функцию: единственное, что меняется от
    секции к секции, — радиусы и высота области. Ни id, ни словари здесь не трогаются: id
    раздаются одним диапазоном в build_scene по итоговой длине колонок, а имена интернируются
    в общие _Pool — поэтому вторая и третья секции ничего не ломают у первой.
    """
    for _ in range(section.count):
        pos = sample_point(section.inner_radius, section.outer_radius, section.fill)
        rot = random_rotation()
        scale = random.uniform(CUBE_SCALE_MIN, CUBE_SCALE_MAX)
        transform = make_transform(pos, rot, scale)
        for k in range(16):
            cols.transform[k].append(_fmt(transform[k]))

        cols.model.append(str(cols.models.intern(random.choice(CUBE_MODELS))))

        # jagged: один материал на куб, в ячейке — индекс в словаре materials
        mat = random.choice(CUBE_MATERIALS)
        cols.material.append('[[{}]]'.format(','.join(str(cols.materials.intern(mat + s)) for s in CUBE_LEVEL_SUFFIXES)))

        vx, vy, vz = orbital_velocity(pos)
        cols.vx.append(_fmt(vx))
        cols.vy.append(_fmt(vy))
        cols.vz.append(_fmt(vz))


def emit_jets(center_pos, center_id, cols):
    """Досыпает кубы двух джетов одного центра в колонки cols (свои, не колонки секций: у
    кубов джета другой архетип). Конусы чередуются через куб — делятся поровну. center_id
    уходит в Jet.center — по нему игра находит центр куба."""
    cx, cy, cz = center_pos
    mat_row = '[[{}]]'.format(','.join(str(cols.materials.intern(JET_MATERIAL + s)) for s in CUBE_LEVEL_SUFFIXES))
    for i in range(JET_COUNT):
        dx, dy, dz = jet_direction(1.0 if i % 2 == 0 else -1.0)
        t = random.uniform(0.0, JET_RANGE)
        pos = (cx + dx * t, cy + dy * t, cz + dz * t)

        scale = random.uniform(CUBE_SCALE_MIN, CUBE_SCALE_MAX)
        transform = make_transform(pos, random_rotation(), scale)
        for k in range(16):
            cols.transform[k].append(_fmt(transform[k]))

        cols.model.append(str(cols.models.intern(random.choice(CUBE_MODELS))))
        cols.material.append(mat_row)

        cols.vx.append(_fmt(dx * JET_SPEED))
        cols.vy.append(_fmt(dy * JET_SPEED))
        cols.vz.append(_fmt(dz * JET_SPEED))
        cols.jet_center.append(str(center_id))


# Ключи архетипов = отсортированные по алфавиту имена компонентов через запятую (так их строит
# SaveScene движка). Держим их константами: по ним же определяется порядок блоков в файле.
CUBES_ARCHETYPE = "Renderable,Shadow,Transform,Velocity"
CENTER_ARCHETYPE = "Gravity,Renderable,Transform"
JETS_ARCHETYPE = "Jet,Renderable,Transform,Velocity"
SKYBOX_ARCHETYPE = "Renderable"
GRAVITY_WORLD_ARCHETYPE = "GravityWorld"


def _renderable_obj(n, model_cells, material_rows):
    """Тело Renderable. model_cells — индексы моделей строками, material_rows — строки-массивы частей
    '[[L0, L1, ...], ...]' (по списку материалов уровней на часть)."""
    return ",".join([
        _num_col("visible", ["true"] * n),
        _num_col("alpha", ["1"] * n),
        _num_col("flags", ["0"] * n),
        _num_col("model", model_cells),
        _num_col("materials", material_rows),
    ])


def _gravity_center_block(entity_id, center_id, cols):
    """Блок архетипа Gravity,Renderable,Transform — сама сущность-центр (одна штука).

    Имена компонентов идут по алфавиту: тем же порядком их пишет SaveScene движка, так что
    пересохранение сцены из редактора не переставляет ключи в файле.
    """
    ident = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]
    transform = make_transform(GRAVITY_CENTER_POS, ident, GRAVITY_CENTER_SCALE)
    transform_obj = ",".join(_num_col(TRANSFORM_COLS[k], [_fmt(transform[k])]) for k in range(16))
    gravity_obj = ",".join([_num_col("gm", [_fmt(GM)]), _num_col("id", [str(center_id)]),
                            _num_col("core_radius", [_fmt(GRAVITY_CORE_RADIUS)])])
    rend_obj = _renderable_obj(1, [str(cols.models.intern(GRAVITY_CENTER_MODEL))],
                               ['[[' + str(cols.materials.intern(GRAVITY_CENTER_MATERIAL)) + ']]'])
    return ('"' + CENTER_ARCHETYPE + '":{'
            '"count":1,'
            '"entities":[' + str(entity_id) + '],'
            '"Gravity":{' + gravity_obj + '},'
            '"Renderable":{' + rend_obj + '},'
            '"Transform":{' + transform_obj + '}}')


def _light_block():
    """Блок архетипа DirectLight,ShadowCaster (одна сущность, id 0)."""
    dx, dy, dz = DIRECT_LIGHT_DIR
    r, g, b = DIRECT_LIGHT_COLOR
    he = _fmt(max(sec.outer_radius for sec in SECTIONS))   # шаровой слой целиком влезает в этот радиус
    dl = ",".join([
        _num_col("dir_x", [_fmt(dx)]), _num_col("dir_y", [_fmt(dy)]), _num_col("dir_z", [_fmt(dz)]),
        _num_col("r", [_fmt(r)]), _num_col("g", [_fmt(g)]), _num_col("b", [_fmt(b)]),
        _num_col("power", [_fmt(DIRECT_LIGHT_POWER)]),
        _num_col("center_x", ["0"]), _num_col("center_y", ["0"]), _num_col("center_z", ["0"]),
        _num_col("half_extent", [he]), _num_col("half_depth", [he]),
        _num_col("cascade_count", [str(int(LIGHT_CASCADE_COUNT))]),
        _num_col("cascade_ratio", [_fmt(LIGHT_CASCADE_RATIO)]),
    ])
    return ('"DirectLight,ShadowCaster":{'
            '"count":1,"entities":[0],'
            '"DirectLight":{' + dl + '},'
            '"ShadowCaster":{}}')


def _gravity_world_block(entity_id):
    world_obj = ",".join([_num_col("sim_dt", [_fmt(SIM_DT)]), _num_col("jet_return_distance", [_fmt(JET_RANGE)])])
    return ('"' + GRAVITY_WORLD_ARCHETYPE + '":{'
            '"count":1,'
            '"entities":[' + str(entity_id) + '],'
            '"GravityWorld":{' + world_obj + '}}')


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
    # и раскладка диска от неё не меняется. Колонки свои (другой архетип), словари имён — общие.
    jet_cols = _Columns()
    jet_cols.models, jet_cols.materials = cols.models, cols.materials
    # id центра = его номер в списке: у каждого центра свой, по нему джеты ссылаются на центр.
    centers = [GRAVITY_CENTER_POS]
    for center_id, center_pos in enumerate(centers):
        emit_jets(center_pos, center_id, jet_cols)

    n = cols.count()
    n_jets = jet_cols.count()

    # Блоки идут ПО КЛЮЧУ АРХЕТИПА, и id раздаются в том же порядке. Это не косметика: движок
    # в SaveScene сортирует блоки по ключу, а LoadScene нумерует сущности по порядку блоков в
    # файле. Совпасть с этим порядком здесь — значит выдать канонический файл, который первое
    # же пересохранение из редактора не переставит и в котором не поедут id.
    next_id = 1 if EMIT_DIRECT_LIGHT else 0   # свет, если включён, занимает id 0
    center_id = cubes_base = jets_base = skybox_id = world_id = 0
    for key in sorted([CENTER_ARCHETYPE, CUBES_ARCHETYPE, JETS_ARCHETYPE, SKYBOX_ARCHETYPE, GRAVITY_WORLD_ARCHETYPE]):
        if key == CENTER_ARCHETYPE:
            center_id = next_id
            next_id += 1
        elif key == GRAVITY_WORLD_ARCHETYPE:
            world_id = next_id
            next_id += 1
        elif key == SKYBOX_ARCHETYPE:
            skybox_id = next_id
            next_id += 1
        elif key == JETS_ARCHETYPE:
            jets_base = next_id
            next_id += n_jets
        else:
            cubes_base = next_id
            next_id += n

    ids = ",".join(str(cubes_base + i) for i in range(n))

    transform_obj = ",".join(_num_col(TRANSFORM_COLS[k], cols.transform[k]) for k in range(16))
    velocity_obj = ",".join([_num_col("x", cols.vx), _num_col("y", cols.vy), _num_col("z", cols.vz)])
    rend_obj = _renderable_obj(n, cols.model, cols.material)

    cubes_block = ('"' + CUBES_ARCHETYPE + '":{'
                   '"count":' + str(n) + ','
                   '"entities":[' + ids + '],'
                   '"Renderable":{' + rend_obj + '},'
                   '"Shadow":{},'
                   '"Transform":{' + transform_obj + '},'
                   '"Velocity":{' + velocity_obj + '}}')

    jet_ids = ",".join(str(jets_base + i) for i in range(n_jets))
    jet_transform_obj = ",".join(_num_col(TRANSFORM_COLS[k], jet_cols.transform[k]) for k in range(16))
    jet_velocity_obj = ",".join([_num_col("x", jet_cols.vx), _num_col("y", jet_cols.vy), _num_col("z", jet_cols.vz)])
    jets_block = ('"' + JETS_ARCHETYPE + '":{'
                  '"count":' + str(n_jets) + ','
                  '"entities":[' + jet_ids + '],'
                  '"Jet":{' + _num_col("center", jet_cols.jet_center) + '},'
                  '"Renderable":{' + _renderable_obj(n_jets, jet_cols.model, jet_cols.material) + '},'
                  '"Transform":{' + jet_transform_obj + '},'
                  '"Velocity":{' + jet_velocity_obj + '}}')

    # Центр и скайбокс строим ДО сборки шапки: они интернируют свои имена ассетов в те же словари, а они
    # уходят в файл первыми.
    center_block = _gravity_center_block(center_id, 0, cols)   # единственный центр = centers[0]
    skybox_block = ('"' + SKYBOX_ARCHETYPE + '":{'
                    '"count":1,'
                    '"entities":[' + str(skybox_id) + '],'
                    '"Renderable":{' + _renderable_obj(1, [str(cols.models.intern(SKYBOX_MODEL))],
                                                       ['[[' + str(cols.materials.intern(SKYBOX_MATERIAL)) + ']]']) + '}}')

    # Словари — ПЕРВЫМИ: колонки ассетов ссылаются в них индексами. Порядок списков как у
    # движка (std::map → по алфавиту), чтобы пересохранение не переставляло шапку.
    blocks = [cols.materials.json("materials"), cols.models.json("models")]
    # if EMIT_DIRECT_LIGHT:
    #     blocks.append(_light_block())
    by_key = {CENTER_ARCHETYPE: center_block, CUBES_ARCHETYPE: cubes_block, SKYBOX_ARCHETYPE: skybox_block,
              GRAVITY_WORLD_ARCHETYPE: _gravity_world_block(world_id)}
    if n_jets:
        by_key[JETS_ARCHETYPE] = jets_block
    for key in sorted(by_key):
        blocks.append(by_key[key])

    text = "{\n" + ",\n".join(blocks) + "\n}\n"
    return text, cols.models.names, sections, n_jets


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    out_dir = os.path.join(script_dir, SCENE_DIR)
    out_path = os.path.join(out_dir, OUTPUT_NAME)

    if not os.path.isdir(out_dir):
        raise SystemExit("Папки сцены нет: {}\n(ожидается game/saved_scene/scene1M с манифестами ресурсов)".format(out_dir))

    text, used_models, sections, n_jets = build_scene()

    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)

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
    print("Свет (entity 0): {}".format("да" if EMIT_DIRECT_LIGHT else "нет"))
    print("Использовано моделей: {} из {} (cube_0..cube_{}).".format(
        len(used_models), NUM_CUBE_MODELS, NUM_CUBE_MODELS - 1))
    print("Проверь: kCubeVariants в GravityScene.cpp == NUM_CUBE_MODELS ({}), "
          "kGravGM == GM ({}).".format(NUM_CUBE_MODELS, GM))


if __name__ == "__main__":
    main()
