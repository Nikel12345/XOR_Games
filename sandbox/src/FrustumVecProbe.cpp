// ============================================================================
//  Sandbox: фрустум-отсев 1М объектов на CPU через ECS — векторизуется ли он.
//
//  Зачем. Отсев живёт на GPU (culling_pib.comp.hlsl), и там он выдаёт индирект.
//  Процессору иногда нужен тот же тест, но СПИСКОМ: по видимым объектам ходит
//  игровая логика (LOD, стриминг, звук, выбор мышью), а сжатый out_pib с GPU не
//  забрать. Вопрос ровно один: во что обходится такой проход на 1М объектов и
//  какая его форма векторизуется.
//
//  ВАЖНО про происхождение. Зонд НЕ строит свои массивы: сущности создаёт
//  настоящий ObjectManager::CreateEntity, обход — настоящий ForEachArchetype,
//  математика — та же, что в SphereVisible (плоскости Гриббa-Хартмана из строк
//  VP; радиус = радиус модели x наибольшая длина столбца 3x3 трансформа).
//  Проверяется система движка, а не компилятор на синтетике.
//
//  Проход распадается на ДВА, и мерить их надо порознь: горячий тест (вектор) и
//  сборка id (сжатие в список). Уроки GravityVecProbe — ветка, ширина индекса,
//  условная запись — взяты как данность и проверены здесь заново:
//    F0  буквальный перенос шейдера: плоскости строятся и нормируются на каждый
//        объект, ранний выход из теста, запись id прямо в горячем цикле
//    F1  плоскости сняты и нормированы ОДИН раз; ветка и запись на месте
//    F2  без ветвления: запас до ближайшей плоскости пишется значением, сборка
//        id вынесена во второй проход. Индекс size_t, колонки через std::vector
//    F3  то же + сырые указатели __restrict и индекс int
//    F4  F3, но радиус ровно как в шейдере: три sqrt по столбцам
//    F5  AVX2+FMA вручную: потолок теста при том же объёме данных
//    F6  мировая сфера колонками: 4 float на объект вместо 16
//  и сборка списка теми же данными (F0-F6 включают её в форме F7):
//    F7  ветвлением, как пишется само
//    F8  без ветвления: запись безусловна, счётчик двигается на 0 или 1
//    F9  AVX2: левый пак восьми id за шаг по таблице масок
//    F10 всё вместе: тест F5 + сборка F9
//    F11 то же с кэшированной мировой сферой: тест F6 + сборка F9
//
//  ЗАМЕРЫ (i5-7400, 1М объектов, видимы 39%): 40.1 мс наивно -> 2.2 мс (F11),
//  список сущностей при этом у всех вариантов совпадает до последнего id.
//  Что из этого следует:
//    - форма теста стоит 3.6x: ветка и камеро-постоянная работа внутри цикла
//      (F0 40.1) против записи значением (F3 11.0). Ручной AVX снимает ещё
//      четверть (F5 8.7) — автовекторизация даёт SSE, /arch:AVX2 не поднят;
//    - на 39% видимых СБОРКА ВЕТВЛЕНИЕМ дороже самого теста: 3.9 мс (F7) против
//      1.8 мс чистого теста в F6. Предсказатель на такой доле бессилен, и
//      безветвленная сборка снимает шесть седьмых (F8 0.63), левый пак — ещё
//      сорок процентов (F9 0.38);
//    - дальше упирается в ЧТЕНИЕ: тест из матрицы берёт 64 байта на объект
//      (F10 5.3), из кэшированной мировой сферы — 16 (F11 2.2). Это и есть
//      потолок: считать быстрее, чем читать, нечем.
//
//  ПОЛНЫЙ ЗАХВАТ (вторая таблица, выживают все 1М): цена прохода почти не
//  меняется — F10 5.3, F11 2.4, атомиков и конкуренции тут нет. Ветвистая
//  сборка, наоборот, ускоряется вчетверо (F7 3.8 -> 0.64): предсказателю снова
//  есть что угадывать, и это подтверждает, что дорога была НЕПРЕДСКАЗУЕМОСТЬ.
//  Единственная привязка к GPU на сегодня — старый замер юзера на той же сцене:
//  при полном захвате fps 40 -> 35, то есть +3.6 мс на кадр. Это ПОТОЛОК сверху
//  для compute-отсева: в дельту входит и растеризация ставшего видимым миллиона.
//  Прохода culling_pib в изоляции не мерил никто: EngineProfiler считает CPU, а
//  timestamp-запросов в SDL GPU нет.
//
//  ЧЕТЫРЕ КАМЕРЫ (третья таблица): амортизация чтения по камерам — приём из
//  шейдера (цикл по блокам региона на одну загрузку трансформа) — на CPU даёт
//  5%: 8.18 мс одним проходом против 8.61 четырьмя. Общий ВХОД экономится, а
//  покамерный ВЫХОД (свой массив запасов + своя сборка) нет, и он-то и стоит.
//  На GPU соотношение обратное — выход это атомик и 4 байта скаттера, вход 64
//  байта матрицы, — поэтому там тот же цикл экономит именно дорогое. Отсюда
//  цена лишней камеры: у GPU почти нулевая, у CPU линейная. Это и есть довод в
//  пользу GPU-отсева, а не скорость теста: она у сторон одного порядка.
//
//  Вердикт векторизации смотреть в отчёте сборки: python scripts/vec_check.py
//  (метки VEC_HOT).
// ============================================================================
#include "PCH.h"
#include "BaseComponents.h"
#include "CameraStruct.h"
#include "ComponentStorage.h"
#include "ObjectManager.h"

#include <chrono>
#include <cstdio>
#include <immintrin.h>
#include <random>
#include <vector>

namespace {

constexpr size_t N        = 1000000;
constexpr int    kRepeats = 15;

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// ── Ограничивающая сфера как SoA-компонент ──────────────────────────────────
// На GPU сфера приезжает отдельным буфером по строкам трансформа
// (BoundSphereDataModule), и процессору она нужна тем же способом — колонками.
// Путь «взять модель из Renderable сущности и спросить у ModelManager сферу модели»
// для горячего прохода не годится: это поиск в словаре на каждый объект и
// AoS-чтение посреди float-арифметики, то есть отказ от векторизации по
// построению. Компонент объявлен здесь же: движок для этого не правится.
struct BoundSpheres : SoAProxyAddable<BoundSpheres> {
    using soa_tag = void;
    std::vector<float> cx, cy, cz, r;
    size_t size() const { return cx.size(); }
    auto columns() { return std::tie(cx, cy, cz, r); }
};

struct BoundSphereProxy {
    float cx = 0, cy = 0, cz = 0, r = 1;
    using related_soa = BoundSpheres;

    template<class SoA>
    void emplace_to(SoA& soa) const {
        soa.cx.push_back(cx); soa.cy.push_back(cy); soa.cz.push_back(cz); soa.r.push_back(r);
    }
};

// ── Фрустум ─────────────────────────────────────────────────────────────────
// Шесть плоскостей, уже НОРМИРОВАННЫХ. В шейдере length(planes[p].xyz) стоит
// внутри теста и считается заново на каждый объект — там это дёшево, плоскости
// живут в регистрах потока. На CPU это шесть корней на объект из воздуха: длина
// нормали зависит только от камеры.
struct Frustum { glm::vec4 p[6]; };

Frustum MakeFrustum(const glm::mat4& vp)
{
    // Строки VP, а не столбцы: glm хранит матрицу по столбцам (vp[c][r]), а
    // Гриббу-Хартману нужны строки клип-преобразования — те же vp[3] +- vp[i],
    // из которых строит плоскости SphereVisible.
    auto row = [&vp](int r) { return glm::vec4(vp[0][r], vp[1][r], vp[2][r], vp[3][r]); };
    const glm::vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);

    Frustum f{ { r3 + r0, r3 - r0, r3 + r1, r3 - r1, r3 + r2, r3 - r2 } };
    for (glm::vec4& p : f.p)
        p /= glm::length(glm::vec3(p));
    return f;
}

// ── Колонки архетипа ────────────────────────────────────────────────────────
// Буквы Positions идут по строкам, поэтому столбцы матрицы — (x,y,z), (a,b,c),
// (e,f,g), а трансляция — (w,d,h); ровно так их раскладывает и
// TransformDataModule::LoadPositionMatrix.
struct Cols {
    const float *mx, *my, *mz;
    const float *ma, *mb, *mc;
    const float *me, *mf, *mg;
    const float *tw, *td, *th;
    const float *scx, *scy, *scz, *sr;
};

Cols TakeCols(Positions& P, BoundSpheres& S)
{
    return Cols{
        P.x.data(), P.y.data(), P.z.data(),
        P.a.data(), P.b.data(), P.c.data(),
        P.e.data(), P.f.data(), P.g.data(),
        P.w.data(), P.d.data(), P.h.data(),
        S.cx.data(), S.cy.data(), S.cz.data(), S.r.data() };
}

// ── Выход и рабочие массивы ─────────────────────────────────────────────────
// Выход — сырой буфер с ёмкостью N+8, а не вектор с push_back: AVX-сборка пишет
// восемь id за раз и продвигает счётчик на число выживших, то есть последняя
// запись всегда вылезает за конец полезных данных.
std::vector<Entity> g_out;
size_t              g_count = 0;

std::vector<float>  g_margin;     // запас до ближайшей плоскости; >= 0 — видим
std::vector<float>  g_wcx, g_wcy, g_wcz, g_wr;   // мировая сфера объекта (F6)

// ── Сборка списка: три формы ────────────────────────────────────────────────
// На этой сцене видим каждый третий объект — худший случай для предсказателя:
// исход ветки не угадывается ничем. Отсюда три варианта.
size_t CollectBranch(const float* __restrict margin, const Entity* ents, Entity* out, size_t cnt, int n)
{
    for (int i = 0; i < n; ++i)
        if (margin[i] >= 0.0f) out[cnt++] = ents[i];
    return cnt;
}

// Перехода нет: запись безусловна, а счётчик двигается на 0 или 1. Невидимый
// объект просто перезапишется следующим. Векторизовать это нельзя (адрес записи
// зависит от предыдущих итераций), но и предсказывать больше нечего.
size_t CollectBranchless(const float* __restrict margin, const Entity* ents, Entity* out, size_t cnt, int n)
{
    for (int i = 0; i < n; ++i) {
        out[cnt] = ents[i];
        cnt += (margin[i] >= 0.0f) ? 1u : 0u;
    }
    return cnt;
}

// Левый пак: маска сравнения восьми запасов — индекс в таблице перестановок,
// permutevar8x32 сдвигает выживших к началу регистра, счётчик двигается на их
// число. Записывается всегда восемь id, лишние затрёт следующий шаг — ради
// этого у выходного буфера и есть запас в 8 элементов.
alignas(32) int32_t g_pack_lut[256][8];

void BuildPackLut()
{
    for (int m = 0; m < 256; ++m) {
        int k = 0;
        for (int b = 0; b < 8; ++b)
            if (m & (1 << b)) g_pack_lut[m][k++] = b;
        for (; k < 8; ++k) g_pack_lut[m][k] = 0;
    }
}

size_t CollectAvx(const float* margin, const Entity* ents, Entity* out, size_t cnt, int n)
{
    const __m256 zero = _mm256_setzero_ps();
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256 m = _mm256_loadu_ps(margin + i);
        const int mask = _mm256_movemask_ps(_mm256_cmp_ps(m, zero, _CMP_GE_OQ));

        const __m256i ids  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ents + i));
        const __m256i perm = _mm256_load_si256(reinterpret_cast<const __m256i*>(g_pack_lut[mask]));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + cnt), _mm256_permutevar8x32_epi32(ids, perm));

        cnt += _mm_popcnt_u32(static_cast<unsigned>(mask));
    }
    for (; i < n; ++i) {
        out[cnt] = ents[i];
        cnt += (margin[i] >= 0.0f) ? 1u : 0u;
    }
    return cnt;
}

// ── Горячий тест ────────────────────────────────────────────────────────────
// Ширина индукционной переменной обязана совпадать с шириной элемента (иначе
// причина 1104), а шестнадцать колонок без __restrict векторизатор обязан
// считать пересекающимися.
void MarginPass(const Cols& c, const Frustum& F, float* margin_out, int n)
{
    const float* __restrict mx = c.mx;  const float* __restrict my = c.my;  const float* __restrict mz = c.mz;
    const float* __restrict ma = c.ma;  const float* __restrict mb = c.mb;  const float* __restrict mc = c.mc;
    const float* __restrict me = c.me;  const float* __restrict mf = c.mf;  const float* __restrict mg = c.mg;
    const float* __restrict tw = c.tw;  const float* __restrict td = c.td;  const float* __restrict th = c.th;
    const float* __restrict scx = c.scx; const float* __restrict scy = c.scy;
    const float* __restrict scz = c.scz; const float* __restrict sr = c.sr;
    float* __restrict margin = margin_out;

    const float p0x = F.p[0].x, p0y = F.p[0].y, p0z = F.p[0].z, p0w = F.p[0].w;
    const float p1x = F.p[1].x, p1y = F.p[1].y, p1z = F.p[1].z, p1w = F.p[1].w;
    const float p2x = F.p[2].x, p2y = F.p[2].y, p2z = F.p[2].z, p2w = F.p[2].w;
    const float p3x = F.p[3].x, p3y = F.p[3].y, p3z = F.p[3].z, p3w = F.p[3].w;
    const float p4x = F.p[4].x, p4y = F.p[4].y, p4z = F.p[4].z, p4w = F.p[4].w;
    const float p5x = F.p[5].x, p5y = F.p[5].y, p5z = F.p[5].z, p5w = F.p[5].w;

    VEC_HOT("frustum_margin");
    for (int i = 0; i < n; ++i) {
        const float sx = scx[i], sy = scy[i], sz = scz[i];
        const float cx = mx[i] * sx + ma[i] * sy + me[i] * sz + tw[i];
        const float cy = my[i] * sx + mb[i] * sy + mf[i] * sz + td[i];
        const float cz = mz[i] * sx + mc[i] * sy + mg[i] * sz + th[i];

        // Масштаб — длина наибольшего столбца 3x3, и корень здесь ОДИН: sqrt
        // монотонен, поэтому max длин = sqrt(max квадратов) точно, а не
        // приближённо. F4 — та же величина тремя корнями, ради цены.
        const float l0 = mx[i] * mx[i] + my[i] * my[i] + mz[i] * mz[i];
        const float l1 = ma[i] * ma[i] + mb[i] * mb[i] + mc[i] * mc[i];
        const float l2 = me[i] * me[i] + mf[i] * mf[i] + mg[i] * mg[i];
        const float la = l0 > l1 ? l0 : l1;
        const float r  = sr[i] * std::sqrt(la > l2 ? la : l2);

        float m = p0x * cx + p0y * cy + p0z * cz + p0w;
        float t = p1x * cx + p1y * cy + p1z * cz + p1w;  m = t < m ? t : m;
        t       = p2x * cx + p2y * cy + p2z * cz + p2w;  m = t < m ? t : m;
        t       = p3x * cx + p3y * cy + p3z * cz + p3w;  m = t < m ? t : m;
        t       = p4x * cx + p4y * cy + p4z * cz + p4w;  m = t < m ? t : m;
        t       = p5x * cx + p5y * cy + p5z * cz + p5w;  m = t < m ? t : m;

        margin[i] = m + r;
    }
}

// Восемь объектов за шаг, умножение со сложением одной инструкцией. Хвост —
// скаляром: он короче вектора и на общее время не влияет.
void MarginPassAvx(const Cols& c, const Frustum& F, float* margin, int n)
{
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256 vx = _mm256_loadu_ps(c.mx + i), vy = _mm256_loadu_ps(c.my + i), vz = _mm256_loadu_ps(c.mz + i);
        const __m256 va = _mm256_loadu_ps(c.ma + i), vb = _mm256_loadu_ps(c.mb + i), vc = _mm256_loadu_ps(c.mc + i);
        const __m256 ve = _mm256_loadu_ps(c.me + i), vf = _mm256_loadu_ps(c.mf + i), vg = _mm256_loadu_ps(c.mg + i);
        const __m256 sx = _mm256_loadu_ps(c.scx + i), sy = _mm256_loadu_ps(c.scy + i), sz = _mm256_loadu_ps(c.scz + i);

        const __m256 cx = _mm256_fmadd_ps(vx, sx, _mm256_fmadd_ps(va, sy, _mm256_fmadd_ps(ve, sz, _mm256_loadu_ps(c.tw + i))));
        const __m256 cy = _mm256_fmadd_ps(vy, sx, _mm256_fmadd_ps(vb, sy, _mm256_fmadd_ps(vf, sz, _mm256_loadu_ps(c.td + i))));
        const __m256 cz = _mm256_fmadd_ps(vz, sx, _mm256_fmadd_ps(vc, sy, _mm256_fmadd_ps(vg, sz, _mm256_loadu_ps(c.th + i))));

        const __m256 l0 = _mm256_fmadd_ps(vx, vx, _mm256_fmadd_ps(vy, vy, _mm256_mul_ps(vz, vz)));
        const __m256 l1 = _mm256_fmadd_ps(va, va, _mm256_fmadd_ps(vb, vb, _mm256_mul_ps(vc, vc)));
        const __m256 l2 = _mm256_fmadd_ps(ve, ve, _mm256_fmadd_ps(vf, vf, _mm256_mul_ps(vg, vg)));
        const __m256 r  = _mm256_mul_ps(_mm256_loadu_ps(c.sr + i),
            _mm256_sqrt_ps(_mm256_max_ps(_mm256_max_ps(l0, l1), l2)));

        __m256 m = _mm256_fmadd_ps(_mm256_set1_ps(F.p[0].x), cx,
                   _mm256_fmadd_ps(_mm256_set1_ps(F.p[0].y), cy,
                   _mm256_fmadd_ps(_mm256_set1_ps(F.p[0].z), cz, _mm256_set1_ps(F.p[0].w))));
        for (int p = 1; p < 6; ++p) {
            const __m256 t = _mm256_fmadd_ps(_mm256_set1_ps(F.p[p].x), cx,
                             _mm256_fmadd_ps(_mm256_set1_ps(F.p[p].y), cy,
                             _mm256_fmadd_ps(_mm256_set1_ps(F.p[p].z), cz, _mm256_set1_ps(F.p[p].w))));
            m = _mm256_min_ps(m, t);
        }

        _mm256_storeu_ps(margin + i, _mm256_add_ps(m, r));
    }

    for (; i < n; ++i) {
        const float sx = c.scx[i], sy = c.scy[i], sz = c.scz[i];
        const float cx = c.mx[i] * sx + c.ma[i] * sy + c.me[i] * sz + c.tw[i];
        const float cy = c.my[i] * sx + c.mb[i] * sy + c.mf[i] * sz + c.td[i];
        const float cz = c.mz[i] * sx + c.mc[i] * sy + c.mg[i] * sz + c.th[i];

        const float l0 = c.mx[i] * c.mx[i] + c.my[i] * c.my[i] + c.mz[i] * c.mz[i];
        const float l1 = c.ma[i] * c.ma[i] + c.mb[i] * c.mb[i] + c.mc[i] * c.mc[i];
        const float l2 = c.me[i] * c.me[i] + c.mf[i] * c.mf[i] + c.mg[i] * c.mg[i];
        const float la = l0 > l1 ? l0 : l1;
        const float r  = c.sr[i] * std::sqrt(la > l2 ? la : l2);

        float m = 1e30f;
        for (int p = 0; p < 6; ++p) {
            const float t = F.p[p].x * cx + F.p[p].y * cy + F.p[p].z * cz + F.p[p].w;
            m = t < m ? t : m;
        }
        margin[i] = m + r;
    }
}

// Мировой центр и радиус уже готовы: читается 4 float на объект вместо 16, и от
// арифметики остаются шесть скалярных произведений.
void MarginPassWorld(const float* wcx, const float* wcy, const float* wcz, const float* wr,
                     const Frustum& F, float* margin_out, int n)
{
    const float* __restrict cxs = wcx;
    const float* __restrict cys = wcy;
    const float* __restrict czs = wcz;
    const float* __restrict rs  = wr;
    float* __restrict margin = margin_out;

    const float p0x = F.p[0].x, p0y = F.p[0].y, p0z = F.p[0].z, p0w = F.p[0].w;
    const float p1x = F.p[1].x, p1y = F.p[1].y, p1z = F.p[1].z, p1w = F.p[1].w;
    const float p2x = F.p[2].x, p2y = F.p[2].y, p2z = F.p[2].z, p2w = F.p[2].w;
    const float p3x = F.p[3].x, p3y = F.p[3].y, p3z = F.p[3].z, p3w = F.p[3].w;
    const float p4x = F.p[4].x, p4y = F.p[4].y, p4z = F.p[4].z, p4w = F.p[4].w;
    const float p5x = F.p[5].x, p5y = F.p[5].y, p5z = F.p[5].z, p5w = F.p[5].w;

    VEC_HOT("frustum_world_sphere");
    for (int i = 0; i < n; ++i) {
        const float cx = cxs[i], cy = cys[i], cz = czs[i];

        float m = p0x * cx + p0y * cy + p0z * cz + p0w;
        float t = p1x * cx + p1y * cy + p1z * cz + p1w;  m = t < m ? t : m;
        t       = p2x * cx + p2y * cy + p2z * cz + p2w;  m = t < m ? t : m;
        t       = p3x * cx + p3y * cy + p3z * cz + p3w;  m = t < m ? t : m;
        t       = p4x * cx + p4y * cy + p4z * cz + p4w;  m = t < m ? t : m;
        t       = p5x * cx + p5y * cy + p5z * cz + p5w;  m = t < m ? t : m;

        margin[i] = m + rs[i];
    }
}

// ── Несколько камер за один проход ──────────────────────────────────────────
// Главный структурный козырь шейдера: трансформ и сфера читаются ОДИН раз, а
// цикл по блокам региона гоняет по ним все камеры прохода. Здесь тот же приём
// на CPU — четыре фрустума на одну загрузку сферы, — чтобы увидеть, амортизация
// это свойство GPU или свойство задачи.
constexpr int K_CAMS = 4;

void MarginPassWorldMulti(const float* wcx, const float* wcy, const float* wcz, const float* wr,
                          const Frustum* FS, float* m0_out, float* m1_out, float* m2_out, float* m3_out, int n)
{
    const float* __restrict cxs = wcx;
    const float* __restrict cys = wcy;
    const float* __restrict czs = wcz;
    const float* __restrict rs  = wr;
    float* __restrict o0 = m0_out;
    float* __restrict o1 = m1_out;
    float* __restrict o2 = m2_out;
    float* __restrict o3 = m3_out;

    // Четыре камеры x шесть плоскостей — 96 констант, в регистры они не влезут.
    // Векторизатору это не мешает: они читаются из стека как скаляры-броадкасты,
    // а трафик на объект остаётся тем же, что у одной камеры.
    const Frustum f0 = FS[0], f1 = FS[1], f2 = FS[2], f3 = FS[3];

    VEC_HOT("frustum_world_4cam");
    for (int i = 0; i < n; ++i) {
        const float cx = cxs[i], cy = cys[i], cz = czs[i], r = rs[i];

        float a = f0.p[0].x * cx + f0.p[0].y * cy + f0.p[0].z * cz + f0.p[0].w;
        float b = f1.p[0].x * cx + f1.p[0].y * cy + f1.p[0].z * cz + f1.p[0].w;
        float c = f2.p[0].x * cx + f2.p[0].y * cy + f2.p[0].z * cz + f2.p[0].w;
        float d = f3.p[0].x * cx + f3.p[0].y * cy + f3.p[0].z * cz + f3.p[0].w;

        // Плоскости 1..5 развёрнуты руками: вложенный цикл векторизатор не берёт
        // (причина 1106), хотя границы у него константные.
        float t;
        t = f0.p[1].x * cx + f0.p[1].y * cy + f0.p[1].z * cz + f0.p[1].w;  a = t < a ? t : a;
        t = f1.p[1].x * cx + f1.p[1].y * cy + f1.p[1].z * cz + f1.p[1].w;  b = t < b ? t : b;
        t = f2.p[1].x * cx + f2.p[1].y * cy + f2.p[1].z * cz + f2.p[1].w;  c = t < c ? t : c;
        t = f3.p[1].x * cx + f3.p[1].y * cy + f3.p[1].z * cz + f3.p[1].w;  d = t < d ? t : d;

        t = f0.p[2].x * cx + f0.p[2].y * cy + f0.p[2].z * cz + f0.p[2].w;  a = t < a ? t : a;
        t = f1.p[2].x * cx + f1.p[2].y * cy + f1.p[2].z * cz + f1.p[2].w;  b = t < b ? t : b;
        t = f2.p[2].x * cx + f2.p[2].y * cy + f2.p[2].z * cz + f2.p[2].w;  c = t < c ? t : c;
        t = f3.p[2].x * cx + f3.p[2].y * cy + f3.p[2].z * cz + f3.p[2].w;  d = t < d ? t : d;

        t = f0.p[3].x * cx + f0.p[3].y * cy + f0.p[3].z * cz + f0.p[3].w;  a = t < a ? t : a;
        t = f1.p[3].x * cx + f1.p[3].y * cy + f1.p[3].z * cz + f1.p[3].w;  b = t < b ? t : b;
        t = f2.p[3].x * cx + f2.p[3].y * cy + f2.p[3].z * cz + f2.p[3].w;  c = t < c ? t : c;
        t = f3.p[3].x * cx + f3.p[3].y * cy + f3.p[3].z * cz + f3.p[3].w;  d = t < d ? t : d;

        t = f0.p[4].x * cx + f0.p[4].y * cy + f0.p[4].z * cz + f0.p[4].w;  a = t < a ? t : a;
        t = f1.p[4].x * cx + f1.p[4].y * cy + f1.p[4].z * cz + f1.p[4].w;  b = t < b ? t : b;
        t = f2.p[4].x * cx + f2.p[4].y * cy + f2.p[4].z * cz + f2.p[4].w;  c = t < c ? t : c;
        t = f3.p[4].x * cx + f3.p[4].y * cy + f3.p[4].z * cz + f3.p[4].w;  d = t < d ? t : d;

        t = f0.p[5].x * cx + f0.p[5].y * cy + f0.p[5].z * cz + f0.p[5].w;  a = t < a ? t : a;
        t = f1.p[5].x * cx + f1.p[5].y * cy + f1.p[5].z * cz + f1.p[5].w;  b = t < b ? t : b;
        t = f2.p[5].x * cx + f2.p[5].y * cy + f2.p[5].z * cz + f2.p[5].w;  c = t < c ? t : c;
        t = f3.p[5].x * cx + f3.p[5].y * cy + f3.p[5].z * cz + f3.p[5].w;  d = t < d ? t : d;

        o0[i] = a + r;  o1[i] = b + r;  o2[i] = c + r;  o3[i] = d + r;
    }
}

// ── F0: буквальный перенос шейдера ──────────────────────────────────────────
bool SphereVisibleAsInShader(const glm::mat4& vp, const glm::vec3& c, float r)
{
    auto row = [&vp](int i) { return glm::vec4(vp[0][i], vp[1][i], vp[2][i], vp[3][i]); };
    const glm::vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    const glm::vec4 planes[6] = { r3 + r0, r3 - r0, r3 + r1, r3 - r1, r3 + r2, r3 - r2 };

    for (int p = 0; p < 6; ++p) {
        if (glm::dot(glm::vec3(planes[p]), c) + planes[p].w < -r * glm::length(glm::vec3(planes[p])))
            return false;
    }
    return true;
}

// Набор компонентов — Positions + Draw, тот же отбор, по которому идут
// TransformDataModule и BoundSphereDataModule: иначе зонд мерил бы не тот набор
// объектов, который отсекает движок. Само поле visible горячий цикл не читает —
// скрытые сущности выбывают раньше, на сборке батчей (EngineContext::HideEntity).
void F0_ShaderShape(ObjectManager& om, SceneData* scene, const glm::mat4& vp)
{
    om.ForEachArchetype<Positions, BoundSpheres, Renderable>(scene,
        [&](ComponentArray<Positions, void>* pos_arr, ComponentArray<BoundSpheres, void>* sph_arr,
            ComponentArray<Renderable, void>*, const std::vector<Entity>& ents)
    {
        Positions&    P = pos_arr->data;
        BoundSpheres& S = sph_arr->data;
        const size_t n = ents.size();
        Entity* out = g_out.data();
        size_t cnt = g_count;

        for (size_t i = 0; i < n; ++i) {
            const float sx = S.cx[i], sy = S.cy[i], sz = S.cz[i];
            const glm::vec3 c(
                P.x[i] * sx + P.a[i] * sy + P.e[i] * sz + P.w[i],
                P.y[i] * sx + P.b[i] * sy + P.f[i] * sz + P.d[i],
                P.z[i] * sx + P.c[i] * sy + P.g[i] * sz + P.h[i]);

            const float l0 = std::sqrt(P.x[i] * P.x[i] + P.y[i] * P.y[i] + P.z[i] * P.z[i]);
            const float l1 = std::sqrt(P.a[i] * P.a[i] + P.b[i] * P.b[i] + P.c[i] * P.c[i]);
            const float l2 = std::sqrt(P.e[i] * P.e[i] + P.f[i] * P.f[i] + P.g[i] * P.g[i]);
            const float r  = S.r[i] * std::max(l0, std::max(l1, l2));

            if (SphereVisibleAsInShader(vp, c, r)) out[cnt++] = ents[i];
        }
        g_count = cnt;
    });
}

// ── F1: плоскости сняты из цикла ────────────────────────────────────────────
// Снимает подозрение на камеро-постоянную работу внутри тела: шесть плоскостей
// и шесть длин нормалей больше не считаются на объект. Ветка и сборка на месте.
void F1_HoistedPlanes(ObjectManager& om, SceneData* scene, const Frustum& F)
{
    om.ForEachArchetype<Positions, BoundSpheres, Renderable>(scene,
        [&](ComponentArray<Positions, void>* pos_arr, ComponentArray<BoundSpheres, void>* sph_arr,
            ComponentArray<Renderable, void>*, const std::vector<Entity>& ents)
    {
        Positions&    P = pos_arr->data;
        BoundSpheres& S = sph_arr->data;
        const size_t n = ents.size();
        Entity* out = g_out.data();
        size_t cnt = g_count;

        for (size_t i = 0; i < n; ++i) {
            const float sx = S.cx[i], sy = S.cy[i], sz = S.cz[i];
            const float cx = P.x[i] * sx + P.a[i] * sy + P.e[i] * sz + P.w[i];
            const float cy = P.y[i] * sx + P.b[i] * sy + P.f[i] * sz + P.d[i];
            const float cz = P.z[i] * sx + P.c[i] * sy + P.g[i] * sz + P.h[i];

            const float l0 = P.x[i] * P.x[i] + P.y[i] * P.y[i] + P.z[i] * P.z[i];
            const float l1 = P.a[i] * P.a[i] + P.b[i] * P.b[i] + P.c[i] * P.c[i];
            const float l2 = P.e[i] * P.e[i] + P.f[i] * P.f[i] + P.g[i] * P.g[i];
            const float r  = S.r[i] * std::sqrt(std::max(l0, std::max(l1, l2)));

            bool visible = true;
            for (int p = 0; p < 6; ++p) {
                if (F.p[p].x * cx + F.p[p].y * cy + F.p[p].z * cz + F.p[p].w < -r) {
                    visible = false;
                    break;
                }
            }
            if (visible) out[cnt++] = ents[i];
        }
        g_count = cnt;
    });
}

// ── F2: без ветвления, сборка вторым проходом ───────────────────────────────
// Две вещи, которые в горячий цикл не пускают: ранний выход и сжатие в список.
// Обе уходят: цикл пишет ЗАПАС до ближайшей плоскости (знак = ответ), а id
// собирает второй проход. Индекс здесь ещё size_t, колонки — через std::vector:
// хватит ли одного этого. Хватает: цикл векторизуется уже тут, и F3 добавляет
// два процента. В гравитации size_t давал отказ 1104, потому что там колонки
// читались И писались (v += a); здесь чтение и запись — разные массивы.
void F2_MarginPass(ObjectManager& om, SceneData* scene, const Frustum& F)
{
    om.ForEachArchetype<Positions, BoundSpheres, Renderable>(scene,
        [&](ComponentArray<Positions, void>* pos_arr, ComponentArray<BoundSpheres, void>* sph_arr,
            ComponentArray<Renderable, void>*, const std::vector<Entity>& ents)
    {
        Positions&    P = pos_arr->data;
        BoundSpheres& S = sph_arr->data;
        const size_t n = ents.size();
        if (g_margin.size() < n) g_margin.resize(n);

        const float p0x = F.p[0].x, p0y = F.p[0].y, p0z = F.p[0].z, p0w = F.p[0].w;
        const float p1x = F.p[1].x, p1y = F.p[1].y, p1z = F.p[1].z, p1w = F.p[1].w;
        const float p2x = F.p[2].x, p2y = F.p[2].y, p2z = F.p[2].z, p2w = F.p[2].w;
        const float p3x = F.p[3].x, p3y = F.p[3].y, p3z = F.p[3].z, p3w = F.p[3].w;
        const float p4x = F.p[4].x, p4y = F.p[4].y, p4z = F.p[4].z, p4w = F.p[4].w;
        const float p5x = F.p[5].x, p5y = F.p[5].y, p5z = F.p[5].z, p5w = F.p[5].w;

        for (size_t i = 0; i < n; ++i) {
            const float sx = S.cx[i], sy = S.cy[i], sz = S.cz[i];
            const float cx = P.x[i] * sx + P.a[i] * sy + P.e[i] * sz + P.w[i];
            const float cy = P.y[i] * sx + P.b[i] * sy + P.f[i] * sz + P.d[i];
            const float cz = P.z[i] * sx + P.c[i] * sy + P.g[i] * sz + P.h[i];

            const float l0 = P.x[i] * P.x[i] + P.y[i] * P.y[i] + P.z[i] * P.z[i];
            const float l1 = P.a[i] * P.a[i] + P.b[i] * P.b[i] + P.c[i] * P.c[i];
            const float l2 = P.e[i] * P.e[i] + P.f[i] * P.f[i] + P.g[i] * P.g[i];
            const float la = l0 > l1 ? l0 : l1;
            const float r  = S.r[i] * std::sqrt(la > l2 ? la : l2);

            float m = p0x * cx + p0y * cy + p0z * cz + p0w;
            float t = p1x * cx + p1y * cy + p1z * cz + p1w;  m = t < m ? t : m;
            t       = p2x * cx + p2y * cy + p2z * cz + p2w;  m = t < m ? t : m;
            t       = p3x * cx + p3y * cy + p3z * cz + p3w;  m = t < m ? t : m;
            t       = p4x * cx + p4y * cy + p4z * cz + p4w;  m = t < m ? t : m;
            t       = p5x * cx + p5y * cy + p5z * cz + p5w;  m = t < m ? t : m;

            g_margin[i] = m + r;
        }

        g_count = CollectBranch(g_margin.data(), ents.data(), g_out.data(), g_count, safe_size_i(n));
    });
}

// ── F3: указатели __restrict и индекс int ───────────────────────────────────
void F3_RestrictInt(ObjectManager& om, SceneData* scene, const Frustum& F)
{
    om.ForEachArchetype<Positions, BoundSpheres, Renderable>(scene,
        [&](ComponentArray<Positions, void>* pos_arr, ComponentArray<BoundSpheres, void>* sph_arr,
            ComponentArray<Renderable, void>*, const std::vector<Entity>& ents)
    {
        const int n = safe_size_i(ents.size());
        if (n == 0) return;
        if (safe_size_i(g_margin.size()) < n) g_margin.resize(n);

        MarginPass(TakeCols(pos_arr->data, sph_arr->data), F, g_margin.data(), n);
        g_count = CollectBranch(g_margin.data(), ents.data(), g_out.data(), g_count, n);
    });
}

// ── F4: радиус ровно как в шейдере ──────────────────────────────────────────
// Три корня вместо одного. Результат тот же до бита, вопрос только в цене — и в
// том, берёт ли векторизатор три sqrtps подряд.
void F4_ThreeSqrt(ObjectManager& om, SceneData* scene, const Frustum& F)
{
    om.ForEachArchetype<Positions, BoundSpheres, Renderable>(scene,
        [&](ComponentArray<Positions, void>* pos_arr, ComponentArray<BoundSpheres, void>* sph_arr,
            ComponentArray<Renderable, void>*, const std::vector<Entity>& ents)
    {
        const int n = safe_size_i(ents.size());
        if (n == 0) return;
        if (safe_size_i(g_margin.size()) < n) g_margin.resize(n);

        const Cols c = TakeCols(pos_arr->data, sph_arr->data);
        const float* __restrict mx = c.mx;  const float* __restrict my = c.my;  const float* __restrict mz = c.mz;
        const float* __restrict ma = c.ma;  const float* __restrict mb = c.mb;  const float* __restrict mc = c.mc;
        const float* __restrict me = c.me;  const float* __restrict mf = c.mf;  const float* __restrict mg = c.mg;
        const float* __restrict tw = c.tw;  const float* __restrict td = c.td;  const float* __restrict th = c.th;
        const float* __restrict scx = c.scx; const float* __restrict scy = c.scy;
        const float* __restrict scz = c.scz; const float* __restrict sr = c.sr;
        float* __restrict margin = g_margin.data();

        const float p0x = F.p[0].x, p0y = F.p[0].y, p0z = F.p[0].z, p0w = F.p[0].w;
        const float p1x = F.p[1].x, p1y = F.p[1].y, p1z = F.p[1].z, p1w = F.p[1].w;
        const float p2x = F.p[2].x, p2y = F.p[2].y, p2z = F.p[2].z, p2w = F.p[2].w;
        const float p3x = F.p[3].x, p3y = F.p[3].y, p3z = F.p[3].z, p3w = F.p[3].w;
        const float p4x = F.p[4].x, p4y = F.p[4].y, p4z = F.p[4].z, p4w = F.p[4].w;
        const float p5x = F.p[5].x, p5y = F.p[5].y, p5z = F.p[5].z, p5w = F.p[5].w;

        VEC_HOT("frustum_margin_3sqrt");
        for (int i = 0; i < n; ++i) {
            const float sx = scx[i], sy = scy[i], sz = scz[i];
            const float cx = mx[i] * sx + ma[i] * sy + me[i] * sz + tw[i];
            const float cy = my[i] * sx + mb[i] * sy + mf[i] * sz + td[i];
            const float cz = mz[i] * sx + mc[i] * sy + mg[i] * sz + th[i];

            const float l0 = std::sqrt(mx[i] * mx[i] + my[i] * my[i] + mz[i] * mz[i]);
            const float l1 = std::sqrt(ma[i] * ma[i] + mb[i] * mb[i] + mc[i] * mc[i]);
            const float l2 = std::sqrt(me[i] * me[i] + mf[i] * mf[i] + mg[i] * mg[i]);
            const float la = l0 > l1 ? l0 : l1;
            const float r  = sr[i] * (la > l2 ? la : l2);

            float m = p0x * cx + p0y * cy + p0z * cz + p0w;
            float t = p1x * cx + p1y * cy + p1z * cz + p1w;  m = t < m ? t : m;
            t       = p2x * cx + p2y * cy + p2z * cz + p2w;  m = t < m ? t : m;
            t       = p3x * cx + p3y * cy + p3z * cz + p3w;  m = t < m ? t : m;
            t       = p4x * cx + p4y * cy + p4z * cz + p4w;  m = t < m ? t : m;
            t       = p5x * cx + p5y * cy + p5z * cz + p5w;  m = t < m ? t : m;

            margin[i] = m + r;
        }

        g_count = CollectBranch(margin, ents.data(), g_out.data(), g_count, n);
    });
}

// ── F5-F6: тот же тест другими средствами ───────────────────────────────────
void F5_Avx2(ObjectManager& om, SceneData* scene, const Frustum& F)
{
    om.ForEachArchetype<Positions, BoundSpheres, Renderable>(scene,
        [&](ComponentArray<Positions, void>* pos_arr, ComponentArray<BoundSpheres, void>* sph_arr,
            ComponentArray<Renderable, void>*, const std::vector<Entity>& ents)
    {
        const int n = safe_size_i(ents.size());
        if (n == 0) return;
        if (safe_size_i(g_margin.size()) < n) g_margin.resize(n);

        MarginPassAvx(TakeCols(pos_arr->data, sph_arr->data), F, g_margin.data(), n);
        g_count = CollectBranch(g_margin.data(), ents.data(), g_out.data(), g_count, n);
    });
}

void PrepareWorldSpheres(ObjectManager& om, SceneData* scene)
{
    om.ForEachArchetype<Positions, BoundSpheres, Renderable>(scene,
        [&](ComponentArray<Positions, void>* pos_arr, ComponentArray<BoundSpheres, void>* sph_arr,
            ComponentArray<Renderable, void>*, const std::vector<Entity>& ents)
    {
        Positions&    P = pos_arr->data;
        BoundSpheres& S = sph_arr->data;
        const size_t n = ents.size();
        g_wcx.resize(n); g_wcy.resize(n); g_wcz.resize(n); g_wr.resize(n);

        for (size_t i = 0; i < n; ++i) {
            const float sx = S.cx[i], sy = S.cy[i], sz = S.cz[i];
            g_wcx[i] = P.x[i] * sx + P.a[i] * sy + P.e[i] * sz + P.w[i];
            g_wcy[i] = P.y[i] * sx + P.b[i] * sy + P.f[i] * sz + P.d[i];
            g_wcz[i] = P.z[i] * sx + P.c[i] * sy + P.g[i] * sz + P.h[i];

            const float l0 = P.x[i] * P.x[i] + P.y[i] * P.y[i] + P.z[i] * P.z[i];
            const float l1 = P.a[i] * P.a[i] + P.b[i] * P.b[i] + P.c[i] * P.c[i];
            const float l2 = P.e[i] * P.e[i] + P.f[i] * P.f[i] + P.g[i] * P.g[i];
            const float la = l0 > l1 ? l0 : l1;
            g_wr[i] = S.r[i] * std::sqrt(la > l2 ? la : l2);
        }
    });
}

void F6_WorldSphere(ObjectManager& om, SceneData* scene, const Frustum& F)
{
    om.ForEachArchetype<Positions, BoundSpheres, Renderable>(scene,
        [&](ComponentArray<Positions, void>*, ComponentArray<BoundSpheres, void>*,
            ComponentArray<Renderable, void>*, const std::vector<Entity>& ents)
    {
        const int n = safe_size_i(ents.size());
        if (n == 0) return;
        if (safe_size_i(g_margin.size()) < n) g_margin.resize(n);

        MarginPassWorld(g_wcx.data(), g_wcy.data(), g_wcz.data(), g_wr.data(), F, g_margin.data(), n);
        g_count = CollectBranch(g_margin.data(), ents.data(), g_out.data(), g_count, n);
    });
}

// ── F7-F9: сборка списка по готовым запасам ─────────────────────────────────
// Запасы в g_margin оставил предыдущий вариант, поэтому мерится ровно сборка.
template <class Collect>
void CollectOnly(ObjectManager& om, SceneData* scene, Collect&& collect)
{
    om.ForEachArchetype<Positions, BoundSpheres, Renderable>(scene,
        [&](ComponentArray<Positions, void>*, ComponentArray<BoundSpheres, void>*,
            ComponentArray<Renderable, void>*, const std::vector<Entity>& ents)
    {
        g_count = collect(g_margin.data(), ents.data(), g_out.data(), g_count, safe_size_i(ents.size()));
    });
}

// ── F10-F11: тест и сборка, обе векторные ───────────────────────────────────
void F10_AvxBoth(ObjectManager& om, SceneData* scene, const Frustum& F)
{
    om.ForEachArchetype<Positions, BoundSpheres, Renderable>(scene,
        [&](ComponentArray<Positions, void>* pos_arr, ComponentArray<BoundSpheres, void>* sph_arr,
            ComponentArray<Renderable, void>*, const std::vector<Entity>& ents)
    {
        const int n = safe_size_i(ents.size());
        if (n == 0) return;
        if (safe_size_i(g_margin.size()) < n) g_margin.resize(n);

        MarginPassAvx(TakeCols(pos_arr->data, sph_arr->data), F, g_margin.data(), n);
        g_count = CollectAvx(g_margin.data(), ents.data(), g_out.data(), g_count, n);
    });
}

void F11_WorldAvx(ObjectManager& om, SceneData* scene, const Frustum& F)
{
    om.ForEachArchetype<Positions, BoundSpheres, Renderable>(scene,
        [&](ComponentArray<Positions, void>*, ComponentArray<BoundSpheres, void>*,
            ComponentArray<Renderable, void>*, const std::vector<Entity>& ents)
    {
        const int n = safe_size_i(ents.size());
        if (n == 0) return;
        if (safe_size_i(g_margin.size()) < n) g_margin.resize(n);

        MarginPassWorld(g_wcx.data(), g_wcy.data(), g_wcz.data(), g_wr.data(), F, g_margin.data(), n);
        g_count = CollectAvx(g_margin.data(), ents.data(), g_out.data(), g_count, n);
    });
}

// ── F12/F13: четыре камеры, одним проходом против четырёх ───────────────────
std::vector<float> g_m4[K_CAMS];

void F12_MultiOnePass(ObjectManager& om, SceneData* scene, const Frustum* FS)
{
    om.ForEachArchetype<Positions, BoundSpheres, Renderable>(scene,
        [&](ComponentArray<Positions, void>*, ComponentArray<BoundSpheres, void>*,
            ComponentArray<Renderable, void>*, const std::vector<Entity>& ents)
    {
        const int n = safe_size_i(ents.size());
        if (n == 0) return;
        for (std::vector<float>& m : g_m4)
            if (safe_size_i(m.size()) < n) m.resize(n);

        MarginPassWorldMulti(g_wcx.data(), g_wcy.data(), g_wcz.data(), g_wr.data(), FS,
            g_m4[0].data(), g_m4[1].data(), g_m4[2].data(), g_m4[3].data(), n);

        for (const std::vector<float>& m : g_m4)
            g_count = CollectAvx(m.data(), ents.data(), g_out.data(), g_count, n);
    });
}

void F13_MultiSeparate(ObjectManager& om, SceneData* scene, const Frustum* FS)
{
    for (int k = 0; k < K_CAMS; ++k)
        F11_WorldAvx(om, scene, FS[k]);
}

// ── обвязка ─────────────────────────────────────────────────────────────────
size_t   g_ref_count = 0;
uint64_t g_ref_sum   = 0;
bool     g_ref_taken = false;

// Между таблицами эталон сбрасывается: у другой камеры другой список, и сверять
// его с прежним нечего.
void ResetRef() { g_ref_taken = false; }

template <class Fn>
void Bench(const char* name, size_t bytes_per_obj, Fn&& fn)
{
    double best = 1e30, sum = 0.0;
    for (int r = 0; r < kRepeats; ++r) {
        g_count = 0;
        const auto t = Clock::now();
        fn();
        const double ms = MsSince(t);
        best = ms < best ? ms : best;
        sum += ms;
    }

    uint64_t ids = 0;
    for (size_t i = 0; i < g_count; ++i) ids += g_out[i];

    // Все варианты обязаны отдать ОДИН И ТОТ ЖЕ список: алгебраически они
    // совпадают, и расхождение означало бы ошибку, а не другую точность.
    const char* mark = "";
    if (!g_ref_taken) { g_ref_count = g_count; g_ref_sum = ids; g_ref_taken = true; }
    else if (g_count != g_ref_count || ids != g_ref_sum) mark = "<- РАСХОЖДЕНИЕ";

    const double gb = static_cast<double>(N) * static_cast<double>(bytes_per_obj) / (1 << 30);
    std::printf("  %-32s best %6.3f ms  avg %6.3f ms  %5.1f GB/s  %2zu b/obj  видимых %7zu %s\n",
        name, best, sum / kRepeats, gb / (best / 1000.0), bytes_per_obj, g_count, mark);
    std::fflush(stdout);
}

} // namespace

int main(int, char**)
{
    std::printf("\n=== FrustumVecProbe: N = %zu объектов, %d прогонов на вариант ===\n", N, kRepeats);
    std::printf("    Обход - настоящий ObjectManager::ForEachArchetype<Positions, BoundSpheres, Renderable>.\n");
    std::printf("    Тест - тот же, что в culling_pib.comp.hlsl. AVX2+FMA: %s.\n",
        (SDL_HasAVX2() && SDL_HasAVX()) ? "да" : "нет");
    std::printf("    Вердикт векторизации - python scripts/vec_check.py (метки VEC_HOT).\n\n");

    BuildPackLut();

    ObjectManager om;
    om.CreateScene("probe");
    om.SetActiveScene("probe");

    // Сцена: куб 3000x3000x3000 из объектов со СВОИМ поворотом и неравномерным
    // масштабом - иначе зонд мерил бы единичную матрицу, а не трансформ.
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> pos(-1500.0f, 1500.0f);
    std::uniform_real_distribution<float> ang(0.0f, 6.2831853f);
    std::uniform_real_distribution<float> scl(0.5f, 2.5f);

    const float r_model[4] = { 0.87f, 1.4f, 2.6f, 5.0f };

    const auto t_build = Clock::now();
    for (size_t n = 0; n < N; ++n) {
        const float a = ang(rng), s1 = scl(rng), s2 = scl(rng), s3 = scl(rng);
        const float ca = std::cos(a), sa = std::sin(a);

        // Поворот вокруг Y с неравномерным масштабом. Буквы Positions идут по
        // строкам: столбцы матрицы - (x,y,z), (a,b,c), (e,f,g), трансляция - w,d,h.
        PositionProxy16 p{};
        p.x =  ca * s1;  p.y = 0.0f;  p.z = -sa * s1;
        p.a = 0.0f;      p.b = s2;    p.c = 0.0f;
        p.e =  sa * s3;  p.f = 0.0f;  p.g =  ca * s3;
        p.w = pos(rng);  p.d = pos(rng) * 0.25f;  p.h = pos(rng);

        // Четыре «модели»: сфера у каждой своя, иначе колонка радиусов выродилась
        // бы в константу и чтение из неё компилятор бы снял.
        BoundSphereProxy s{};
        s.cy = 0.5f * static_cast<float>(n & 1u);
        s.r  = r_model[n & 3u];

        om.CreateEntity("probe", p, s, RenderableProxy{});
    }
    std::printf("  сборка сцены: %.0f мс (%zu сущностей)\n", MsSince(t_build), N);

    SceneData* scene = om.GetActiveScene();

    // Камера стоит в четверти куба и смотрит вдоль +Z: так во фрустум попадает
    // осмысленная доля объектов, а не единицы и не всё подряд.
    Camera cam(1920.0f, 1080.0f, Camera::DEFAULT_FOV_Y, 0.1f, 5000.0f);
    cam.SetView(glm::vec3(0.0f, 0.0f, -750.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 vp = cam.GetProj() * cam.GetView();
    const Frustum F = MakeFrustum(vp);

    const auto t_prep = Clock::now();
    PrepareWorldSpheres(om, scene);
    std::printf("  подготовка мировых сфер (для F6, F11): %.2f мс\n\n", MsSince(t_prep));

    // K_CAMS x N: в многокамерном варианте выжившие всех камер ложатся в ОДИН
    // буфер подряд, и одного N там не хватает. +8 — AVX-сборка пишет целыми
    // регистрами, последняя запись всегда вылезает за полезный хвост.
    g_out.resize(static_cast<size_t>(K_CAMS) * N + 8);

    std::printf("  -- проход целиком: тест + сборка списка --\n");
    Bench("F0 как в шейдере (ветка+сборка)", 64, [&] { F0_ShaderShape(om, scene, vp); });
    Bench("F1 плоскости сняты из цикла",     64, [&] { F1_HoistedPlanes(om, scene, F); });
    Bench("F2 запас значением, size_t",      68, [&] { F2_MarginPass(om, scene, F); });
    Bench("F3 + __restrict и int",           68, [&] { F3_RestrictInt(om, scene, F); });
    Bench("F4 F3, три sqrt (как в шейдере)", 68, [&] { F4_ThreeSqrt(om, scene, F); });
    Bench("F5 AVX2+FMA вручную",             68, [&] { F5_Avx2(om, scene, F); });
    Bench("F6 мировая сфера колонками",      20, [&] { F6_WorldSphere(om, scene, F); });
    std::printf("\n  -- ТОЛЬКО 2-й проход: теста здесь нет, запасы уже посчитаны выше --\n");
    Bench("F7 сборка id ветвлением",          4, [&] { CollectOnly(om, scene, CollectBranch); });
    Bench("F8 сборка id без ветвления",       4, [&] { CollectOnly(om, scene, CollectBranchless); });
    Bench("F9 сборка id AVX2 (левый пак)",    4, [&] { CollectOnly(om, scene, CollectAvx); });
    std::printf("\n  -- снова проход целиком, обе половины векторные --\n");
    Bench("F10 тест F5 + сборка F9",         68, [&] { F10_AvxBoth(om, scene, F); });
    Bench("F11 тест F6 + сборка F9",         20, [&] { F11_WorldAvx(om, scene, F); });

    // Вторая постановка: сцена влезает в область видимости ЦЕЛИКОМ, выживают все
    // 1М. Для GPU-отсева это худший случай (миллион атомиков по одному адресу
    // команды), и сравнивать CPU с ним надо на тех же данных. Для CPU здесь,
    // наоборот, ветка в сборке снова предсказуема — видно, что 3.8 мс у F7 были
    // ценой НЕПРЕДСКАЗУЕМОСТИ, а не работы.
    Camera cam_all(1920.0f, 1080.0f, Camera::DEFAULT_FOV_Y, 0.1f, 12000.0f);
    cam_all.SetView(glm::vec3(0.0f, 0.0f, -7000.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const Frustum F_all = MakeFrustum(cam_all.GetProj() * cam_all.GetView());

    ResetRef();
    std::printf("\n  -- полный захват: во фрустуме ВСЕ объекты --\n");
    Bench("F5 AVX-тест + сборка ветвлением",  68, [&] { F5_Avx2(om, scene, F_all); });
    Bench("F7 сборка id ветвлением",           4, [&] { CollectOnly(om, scene, CollectBranch); });
    Bench("F8 сборка id без ветвления",        4, [&] { CollectOnly(om, scene, CollectBranchless); });
    Bench("F9 сборка id AVX2 (левый пак)",     4, [&] { CollectOnly(om, scene, CollectAvx); });
    Bench("F10 тест F5 + сборка F9",          68, [&] { F10_AvxBoth(om, scene, F_all); });
    Bench("F11 тест F6 + сборка F9",          20, [&] { F11_WorldAvx(om, scene, F_all); });

    // Третья постановка: четыре камеры — приближение набора «главная + каскады
    // теней», ради которого в шейдере и живёт цикл по блокам региона. Вопрос:
    // амортизация чтения по камерам — свойство GPU или свойство задачи.
    Frustum FS[K_CAMS];
    {
        const glm::vec3 eye[K_CAMS] = {
            { 0.0f, 0.0f, -750.0f }, { 0.0f, 0.0f, 750.0f },
            { -750.0f, 0.0f, 0.0f }, { 750.0f, 0.0f, 0.0f } };
        const glm::vec3 dir[K_CAMS] = {
            { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f },
            { 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f } };
        for (int k = 0; k < K_CAMS; ++k) {
            Camera c(1920.0f, 1080.0f, Camera::DEFAULT_FOV_Y, 0.1f, 5000.0f);
            c.SetView(eye[k], dir[k], glm::vec3(0.0f, 1.0f, 0.0f));
            FS[k] = MakeFrustum(c.GetProj() * c.GetView());
        }
    }

    ResetRef();
    std::printf("\n  -- четыре камеры: цена амортизации чтения --\n");
    Bench("F12 4 камеры ОДНИМ проходом",      32, [&] { F12_MultiOnePass(om, scene, FS); });
    Bench("F13 4 камеры четырьмя проходами",  80, [&] { F13_MultiSeparate(om, scene, FS); });

    std::printf("\n");
    return 0;
}
