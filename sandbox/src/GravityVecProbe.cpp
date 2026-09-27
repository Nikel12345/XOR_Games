// ============================================================================
//  Sandbox: поддаётся ли гравитационный шаг автовекторизации — БЕЗ интринсиков.
//
//  Зачем. Проход по 800k сущностей в Game::SimulateGravity стоит ~9-13 мс, и
//  /Qvec-report:2 говорит «не векторизован» и для текущей версии, и для исходной.
//  Прежде чем перестраивать игровой цикл, нужен голый ответ: цикл такой формы
//  вообще векторизуем здесь — или мешает что-то принципиальное (sqrt, деление,
//  ветка отсечения, доступ к колонкам SoA через ссылку на архетип)?
//
//  ВАЖНО про происхождение. Зонд НЕ строит свои массивы: сущности создаются
//  настоящим ObjectManager::CreateEntity, а шаг идёт через настоящий
//  ObjectManager::ForEach<Positions, Velocities> — ту же all-SoA форму, что в игре
//  (колонки отдаются целиком, лямбда зовётся раз на архетип). Проверяем систему
//  движка, а не компилятор на синтетике: если векторизации мешает сама раскладка
//  Positions или форма обхода, зонд обязан это показать.
//
//  Лесенка вариантов — каждая ступень снимает ровно одно подозрение:
//    V0  как в игре: обход std::vector<Src> + `if (r <= soft) continue`
//    V1  единственный источник в локалях, ветка сохранена
//    V2  то же без ветвления (софтенинг значением: max вместо continue)
//    V3  V2 + #pragma loop(ivdep)
//    V4  V2 + сырые указатели __restrict на колонки
//
//  Цель — оптимизация (векторизовалось / нет), а не абсолютные миллисекунды.
//  Вердикт компилятора смотреть в отчёте сборки (/Qvec-report:2), здесь — время.
// ============================================================================
#include "PCH.h"
#include "BaseComponents.h"
#include "ObjectManager.h"
#include "ComponentStorage.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr size_t N         = 800'000;   // столько же сущностей, сколько в scene1M
constexpr float  kSimDt    = 0.05f;
constexpr float  kGravSoft = 1e-3f;
constexpr float  kGM       = 5000.0f;
constexpr int    kRepeats  = 20;        // прогонов на вариант, берём лучший

// Та же структура, что Game::GravitySource в своей исходной форме (до габаритов рамки).
struct Src { float x, y, z, gm; };

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// ── V0: форма как в игре ────────────────────────────────────────────────────
// Источники — вектор структур, внутри ветка отсечения. Ровно то, что стоит в
// Game::SimulateGravity, и ровно то, на что компилятор отвечает «500».
void StepGameShape(ObjectManager& om, SceneData* scene, const std::vector<Src>& sources)
{
    om.ForEach<Positions, Velocities>(scene, [&sources](Positions& P, Velocities& V)
    {
        const size_t n = P.w.size();
        for (size_t i = 0; i < n; ++i) {
            const float x = P.w[i], y = P.d[i], z = P.h[i];

            float ax = 0.0f, ay = 0.0f, az = 0.0f;
            for (const Src& s : sources) {
                const float dx = s.x - x, dy = s.y - y, dz = s.z - z;
                const float r2 = dx * dx + dy * dy + dz * dz;
                const float r  = std::sqrt(r2);
                if (r <= kGravSoft) continue;
                const float k = s.gm / (r2 * r);
                ax += dx * k; ay += dy * k; az += dz * k;
            }
            V.x[i] += ax * kSimDt;
            V.y[i] += ay * kSimDt;
            V.z[i] += az * kSimDt;

            P.w[i] += V.x[i] * kSimDt;
            P.d[i] += V.y[i] * kSimDt;
            P.h[i] += V.z[i] * kSimDt;
        }
    });
}

// ── V1: источник в локалях, ветка на месте ──────────────────────────────────
// Снимает подозрение на внутренний цикл по вектору структур; control flow остаётся.
void StepHoistedBranch(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEach<Positions, Velocities>(scene, [&s](Positions& P, Velocities& V)
    {
        const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
        const size_t n = P.w.size();
        for (size_t i = 0; i < n; ++i) {
            const float dx = sx - P.w[i], dy = sy - P.d[i], dz = sz - P.h[i];
            const float r2 = dx * dx + dy * dy + dz * dz;
            const float r  = std::sqrt(r2);
            float ax = 0.0f, ay = 0.0f, az = 0.0f;
            if (r > kGravSoft) {
                const float k = gm / (r2 * r);
                ax = dx * k; ay = dy * k; az = dz * k;
            }
            V.x[i] += ax * kSimDt;
            V.y[i] += ay * kSimDt;
            V.z[i] += az * kSimDt;

            P.w[i] += V.x[i] * kSimDt;
            P.d[i] += V.y[i] * kSimDt;
            P.h[i] += V.z[i] * kSimDt;
        }
    });
}

// ── V2: без ветвления ───────────────────────────────────────────────────────
// Отсечение у центра выражено ЗНАЧЕНИЕМ, а не переходом: r2 зажимается снизу, и
// деление всегда конечно. Это и есть «предобработка вместо ветвления».
void StepBranchless(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEach<Positions, Velocities>(scene, [&s](Positions& P, Velocities& V)
    {
        const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
        const float soft2 = kGravSoft * kGravSoft;
        const size_t n = P.w.size();
        for (size_t i = 0; i < n; ++i) {
            const float dx = sx - P.w[i], dy = sy - P.d[i], dz = sz - P.h[i];
            const float r2 = std::max(dx * dx + dy * dy + dz * dz, soft2);
            const float k  = gm / (r2 * std::sqrt(r2));

            V.x[i] += dx * k * kSimDt;
            V.y[i] += dy * k * kSimDt;
            V.z[i] += dz * k * kSimDt;

            P.w[i] += V.x[i] * kSimDt;
            P.d[i] += V.y[i] * kSimDt;
            P.h[i] += V.z[i] * kSimDt;
        }
    });
}

// ── V3: V2 + указание игнорировать предполагаемые зависимости ───────────────
void GravityBodyIvdep(Positions& P, Velocities& V, const Src& s)
{
    const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
    const float soft2 = kGravSoft * kGravSoft;
    const size_t n = P.w.size();
#pragma loop(ivdep)
    for (size_t i = 0; i < n; ++i) {
        const float dx = sx - P.w[i], dy = sy - P.d[i], dz = sz - P.h[i];
        const float r2 = std::max(dx * dx + dy * dy + dz * dz, soft2);
        const float k  = gm / (r2 * std::sqrt(r2));

        V.x[i] += dx * k * kSimDt;
        V.y[i] += dy * k * kSimDt;
        V.z[i] += dz * k * kSimDt;

        P.w[i] += V.x[i] * kSimDt;
        P.d[i] += V.y[i] * kSimDt;
        P.h[i] += V.z[i] * kSimDt;
    }
}

// MSVC не принимает #pragma loop внутри лямбды (C3925), поэтому тело вынесено
// в функцию выше. Обход при этом тот же ForEach — меняется только место цикла.
void StepBranchlessIvdep(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEach<Positions, Velocities>(scene, [&s](Positions& P, Velocities& V) {
        GravityBodyIvdep(P, V, s);
    });
}

// ── V4: V2 + сырые указатели __restrict на колонки ──────────────────────────
// Снимает последнее подозрение: доступ через std::vector и возможный алиасинг
// шести колонок между собой. Обход по-прежнему ForEach — меняется только тело.
void StepRestrict(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEach<Positions, Velocities>(scene, [&s](Positions& P, Velocities& V)
    {
        const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
        const float soft2 = kGravSoft * kGravSoft;
        const size_t n = P.w.size();

        float* __restrict pw = P.w.data();
        float* __restrict pd = P.d.data();
        float* __restrict ph = P.h.data();
        float* __restrict vx = V.x.data();
        float* __restrict vy = V.y.data();
        float* __restrict vz = V.z.data();

        for (size_t i = 0; i < n; ++i) {
            const float dx = sx - pw[i], dy = sy - pd[i], dz = sz - ph[i];
            const float r2 = std::max(dx * dx + dy * dy + dz * dz, soft2);
            const float k  = gm / (r2 * std::sqrt(r2));

            vx[i] += dx * k * kSimDt;
            vy[i] += dy * k * kSimDt;
            vz[i] += dz * k * kSimDt;

            pw[i] += vx[i] * kSimDt;
            pd[i] += vy[i] * kSimDt;
            ph[i] += vz[i] * kSimDt;
        }
    });
}

// ── V5: V4 + индекс int вместо size_t ─────────────────────────
// Причина отказа у V2-V4 — 1104, «присваивания разного размера»: счётчик
// size_t шириной 64 бита против float шириной 32. Векторизатор MSVC требует,
// чтобы ширина индукционной переменной совпадала с шириной элемента.
void StepRestrictInt(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEach<Positions, Velocities>(scene, [&s](Positions& P, Velocities& V)
    {
        const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
        const float soft2 = kGravSoft * kGravSoft;
        const int n = static_cast<int>(P.w.size());

        float* __restrict pw = P.w.data();
        float* __restrict pd = P.d.data();
        float* __restrict ph = P.h.data();
        float* __restrict vx = V.x.data();
        float* __restrict vy = V.y.data();
        float* __restrict vz = V.z.data();

        for (int i = 0; i < n; ++i) {
            const float dx = sx - pw[i], dy = sy - pd[i], dz = sz - ph[i];
            const float r2 = std::max(dx * dx + dy * dy + dz * dz, soft2);
            const float k  = gm / (r2 * std::sqrt(r2));

            vx[i] += dx * k * kSimDt;
            vy[i] += dy * k * kSimDt;
            vz[i] += dz * k * kSimDt;

            pw[i] += vx[i] * kSimDt;
            pd[i] += vy[i] * kSimDt;
            ph[i] += vz[i] * kSimDt;
        }
    });
}

// ── V6: индекс int, но без __restrict ──────────────────────────
// Отвечает, нужно ли снимать алиасинг руками или хватает одного индекса.
void StepBranchlessInt(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEach<Positions, Velocities>(scene, [&s](Positions& P, Velocities& V)
    {
        const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
        const float soft2 = kGravSoft * kGravSoft;
        const int n = static_cast<int>(P.w.size());
        for (int i = 0; i < n; ++i) {
            const float dx = sx - P.w[i], dy = sy - P.d[i], dz = sz - P.h[i];
            const float r2 = std::max(dx * dx + dy * dy + dz * dz, soft2);
            const float k  = gm / (r2 * std::sqrt(r2));

            V.x[i] += dx * k * kSimDt;
            V.y[i] += dy * k * kSimDt;
            V.z[i] += dz * k * kSimDt;

            P.w[i] += V.x[i] * kSimDt;
            P.d[i] += V.y[i] * kSimDt;
            P.h[i] += V.z[i] * kSimDt;
        }
    });
}

// ── V7/V8: что именно не векторизуется ─────────────────────────
// Код 1104 не снялся ни безветвлением, ни __restrict, ни int-индексом.
// Остались две операции, которые могут его давать: деление и корень. Снимаю их
// по очереди; арифметика при этом теряет смысл — здесь важен только вердикт.
void StepNoSqrtNoDiv(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEach<Positions, Velocities>(scene, [&s](Positions& P, Velocities& V)
    {
        const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
        const int n = static_cast<int>(P.w.size());

        float* __restrict pw = P.w.data();
        float* __restrict pd = P.d.data();
        float* __restrict ph = P.h.data();
        float* __restrict vx = V.x.data();
        float* __restrict vy = V.y.data();
        float* __restrict vz = V.z.data();

        for (int i = 0; i < n; ++i) {
            const float dx = sx - pw[i], dy = sy - pd[i], dz = sz - ph[i];
            const float k = gm * 1e-6f;   // ни корня, ни деления

            vx[i] += dx * k * kSimDt;
            vy[i] += dy * k * kSimDt;
            vz[i] += dz * k * kSimDt;

            pw[i] += vx[i] * kSimDt;
            pd[i] += vy[i] * kSimDt;
            ph[i] += vz[i] * kSimDt;
        }
    });
}

void StepSqrtNoDiv(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEach<Positions, Velocities>(scene, [&s](Positions& P, Velocities& V)
    {
        const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
        const int n = static_cast<int>(P.w.size());

        float* __restrict pw = P.w.data();
        float* __restrict pd = P.d.data();
        float* __restrict ph = P.h.data();
        float* __restrict vx = V.x.data();
        float* __restrict vy = V.y.data();
        float* __restrict vz = V.z.data();

        for (int i = 0; i < n; ++i) {
            const float dx = sx - pw[i], dy = sy - pd[i], dz = sz - ph[i];
            const float r2 = dx * dx + dy * dy + dz * dz;
            const float k  = gm * std::sqrt(r2) * 1e-9f;   // корень есть, деления нет

            vx[i] += dx * k * kSimDt;
            vy[i] += dy * k * kSimDt;
            vz[i] += dz * k * kSimDt;

            pw[i] += vx[i] * kSimDt;
            pd[i] += vy[i] * kSimDt;
            ph[i] += vz[i] * kSimDt;
        }
    });
}

// ── V9: деление без корня ───────────────────────────────────
// Проверка, что дело именно в «/», а не в сочетании с корнем.
void StepDivNoSqrt(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEach<Positions, Velocities>(scene, [&s](Positions& P, Velocities& V)
    {
        const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
        const float soft2 = kGravSoft * kGravSoft;
        const int n = static_cast<int>(P.w.size());

        float* __restrict pw = P.w.data();
        float* __restrict pd = P.d.data();
        float* __restrict ph = P.h.data();
        float* __restrict vx = V.x.data();
        float* __restrict vy = V.y.data();
        float* __restrict vz = V.z.data();

        for (int i = 0; i < n; ++i) {
            const float dx = sx - pw[i], dy = sy - pd[i], dz = sz - ph[i];
            const float r2 = std::max(dx * dx + dy * dy + dz * dz, soft2);
            const float k  = gm / r2;   // деление есть, корня нет

            vx[i] += dx * k * kSimDt;
            vy[i] += dy * k * kSimDt;
            vz[i] += dz * k * kSimDt;

            pw[i] += vx[i] * kSimDt;
            pd[i] += vy[i] * kSimDt;
            ph[i] += vz[i] * kSimDt;
        }
    });
}

// ── V10: обратный корень без деления ──────────────────────────
// Если «/» невекторизуем в принципе, его надо убрать из тела. 1/sqrt(x)
// считается одними умножениями: целочисленная затравка по битам + два шага
// Ньютона (относительная ошибка порядка 1e-6). Интринсиков нет — только
// арифметика и перестановка битов через memcpy.
inline float RsqrtNoDiv(float x)
{
    uint32_t bits;
    std::memcpy(&bits, &x, sizeof bits);
    bits = 0x5f3759dfu - (bits >> 1);
    float y;
    std::memcpy(&y, &bits, sizeof y);
    const float half = 0.5f * x;
    y = y * (1.5f - half * y * y);
    y = y * (1.5f - half * y * y);
    return y;
}

void StepRsqrtNewton(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEach<Positions, Velocities>(scene, [&s](Positions& P, Velocities& V)
    {
        const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
        const float soft2 = kGravSoft * kGravSoft;
        const int n = static_cast<int>(P.w.size());

        float* __restrict pw = P.w.data();
        float* __restrict pd = P.d.data();
        float* __restrict ph = P.h.data();
        float* __restrict vx = V.x.data();
        float* __restrict vy = V.y.data();
        float* __restrict vz = V.z.data();

        for (int i = 0; i < n; ++i) {
            const float dx = sx - pw[i], dy = sy - pd[i], dz = sz - ph[i];
            const float r2 = std::max(dx * dx + dy * dy + dz * dz, soft2);
            const float inv = RsqrtNoDiv(r2);        // 1/r
            const float k = gm * inv * inv * inv;    // gm / r^3, одни умножения

            vx[i] += dx * k * kSimDt;
            vy[i] += dy * k * kSimDt;
            vz[i] += dz * k * kSimDt;

            pw[i] += vx[i] * kSimDt;
            pd[i] += vy[i] * kSimDt;
            ph[i] += vz[i] * kSimDt;
        }
    });
}

// ── V11: контрольный опыт — голое деление ───────────────────────────────────
// Минимальный цикл с одним делением на ПЕРЕМЕННОЕ. Отвечает на вопрос,
// отказывается ли векторизатор от divps как от класса, или дело в остальном теле.
void StepDivOnly(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEach<Positions, Velocities>(scene, [&s](Positions& P, Velocities& V)
    {
        const float gm = s.gm;
        const int n = static_cast<int>(P.w.size());
        const float* __restrict pw = P.w.data();
        float* __restrict vx = V.x.data();
        for (int i = 0; i < n; ++i) {
            vx[i] = gm / pw[i];
        }
    });
}

// ── V12: три прохода с локальным хранилищем ─────────────────────────────────
// Раз деление нельзя векторизовать, его изолируют в СВОЙ проход, чтобы остальные
// два векторизовались. Цена — один лишний массив и ещё один проход по памяти;
// выигрыш будет, только если векторные проходы отыграют больше, чем стоит трафик.
std::vector<float> g_denom;   // локальное хранилище между проходами

void StepThreePass(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEach<Positions, Velocities>(scene, [&s](Positions& P, Velocities& V)
    {
        const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
        const float soft2 = kGravSoft * kGravSoft;
        const int n = static_cast<int>(P.w.size());
        if (static_cast<int>(g_denom.size()) < n) g_denom.resize(n);

        const float* __restrict pw = P.w.data();
        const float* __restrict pd = P.d.data();
        const float* __restrict ph = P.h.data();
        float* __restrict den = g_denom.data();

        // Проход 1 (векторный): знаменатель r^3 = r2 * sqrt(r2).
        for (int i = 0; i < n; ++i) {
            const float dx = sx - pw[i], dy = sy - pd[i], dz = sz - ph[i];
            const float r2 = std::max(dx * dx + dy * dy + dz * dz, soft2);
            den[i] = r2 * std::sqrt(r2);
        }

        // Проход 2 (скалярный по необходимости): только деление, больше ничего.
        for (int i = 0; i < n; ++i) den[i] = gm / den[i];

        // Проход 3 (векторный): применение. dx считаем заново — три вычитания
        // дешевле, чем ещё три массива в памяти.
        float* __restrict pw2 = P.w.data();
        float* __restrict pd2 = P.d.data();
        float* __restrict ph2 = P.h.data();
        float* __restrict vx = V.x.data();
        float* __restrict vy = V.y.data();
        float* __restrict vz = V.z.data();
        for (int i = 0; i < n; ++i) {
            const float k = den[i];
            const float dx = sx - pw2[i], dy = sy - pd2[i], dz = sz - ph2[i];

            vx[i] += dx * k * kSimDt;
            vy[i] += dy * k * kSimDt;
            vz[i] += dz * k * kSimDt;

            pw2[i] += vx[i] * kSimDt;
            pd2[i] += vy[i] * kSimDt;
            ph2[i] += vz[i] * kSimDt;
        }
    });
}

// ── V13: тот же V5, но софтенинг тернарником вместо std::max ────────────────
// Контрольный опыт V11 показал, что деление векторизуется. Единственное, чем
// V2..V6 отличаются от векторизовавшихся V7/V8, — вызов std::max: он берёт и
// возвращает ССЫЛКИ, и векторизатор считает это невекторизуемой операцией (1104).
// Тернарник компилятор сворачивает в maxss/maxps и претензий не имеет.
void StepTernaryClamp(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEach<Positions, Velocities>(scene, [&s](Positions& P, Velocities& V)
    {
        const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
        const float soft2 = kGravSoft * kGravSoft;
        const int n = static_cast<int>(P.w.size());

        float* __restrict pw = P.w.data();
        float* __restrict pd = P.d.data();
        float* __restrict ph = P.h.data();
        float* __restrict vx = V.x.data();
        float* __restrict vy = V.y.data();
        float* __restrict vz = V.z.data();

        for (int i = 0; i < n; ++i) {
            const float dx = sx - pw[i], dy = sy - pd[i], dz = sz - ph[i];
            const float rr = dx * dx + dy * dy + dz * dz;
            const float r2 = rr < soft2 ? soft2 : rr;
            const float k  = gm / (r2 * std::sqrt(r2));

            vx[i] += dx * k * kSimDt;
            vy[i] += dy * k * kSimDt;
            vz[i] += dz * k * kSimDt;

            pw[i] += vx[i] * kSimDt;
            pd[i] += vy[i] * kSimDt;
            ph[i] += vz[i] * kSimDt;
        }
    });
}

// ── V14: то же без __restrict ───────────────────────────────────────────────
// Нужен ли ручной снос алиасинга, если сам блокировщик убран.
void StepTernaryNoRestrict(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEach<Positions, Velocities>(scene, [&s](Positions& P, Velocities& V)
    {
        const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
        const float soft2 = kGravSoft * kGravSoft;
        const int n = static_cast<int>(P.w.size());
        for (int i = 0; i < n; ++i) {
            const float dx = sx - P.w[i], dy = sy - P.d[i], dz = sz - P.h[i];
            const float rr = dx * dx + dy * dy + dz * dz;
            const float r2 = rr < soft2 ? soft2 : rr;
            const float k  = gm / (r2 * std::sqrt(r2));

            V.x[i] += dx * k * kSimDt;
            V.y[i] += dy * k * kSimDt;
            V.z[i] += dz * k * kSimDt;

            P.w[i] += V.x[i] * kSimDt;
            P.d[i] += V.y[i] * kSimDt;
            P.h[i] += V.z[i] * kSimDt;
        }
    });
}

// ── V15/V16: поэлементная форма ForEach через SoAElement ────────────────────
// Вторая форма обхода в движке: цикл по сущностям пишет не вызывающий, а сам
// ForEach, а в лямбду приходит SoAElement<T> — пара {указатель на колонки, индекс}.
// Выбор формы делает ObjectManager::ForEach: при all_soa он отдаёт колонки целиком,
// а поэлементный путь включается, когда лямбда просит Entity первым параметром
// (см. wants_entity в ObjectManager.inl).
//
// Вопрос зонда: видит ли векторизатор такой цикл. Тело здесь — то же, что в V13
// (тернарник, деление, без ветвлений), отличается ТОЛЬКО способ добраться до данных:
// P.container().w[P.i()] вместо сырого указателя.
void StepSoAElement(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEach<Positions, Velocities>(scene,
        [&s](Entity, SoAElement<Positions> pe, SoAElement<Velocities> ve)
    {
        const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
        const float soft2 = kGravSoft * kGravSoft;

        Positions&  P = pe.container();
        Velocities& V = ve.container();
        const size_t i = pe.i();

        const float dx = sx - P.w[i], dy = sy - P.d[i], dz = sz - P.h[i];
        const float rr = dx * dx + dy * dy + dz * dz;
        const float r2 = rr < soft2 ? soft2 : rr;
        const float k  = gm / (r2 * std::sqrt(r2));

        V.x[i] += dx * k * kSimDt;
        V.y[i] += dy * k * kSimDt;
        V.z[i] += dz * k * kSimDt;

        P.w[i] += V.x[i] * kSimDt;
        P.d[i] += V.y[i] * kSimDt;
        P.h[i] += V.z[i] * kSimDt;
    });
}

// V16: то же, но данные берутся через operator SoA& (неявное приведение SoAElement
// к самим колонкам) — проверка, что дело не в способе разыменования.
void StepSoAElementImplicit(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEach<Positions, Velocities>(scene,
        [&s](Entity, SoAElement<Positions> pe, SoAElement<Velocities> ve)
    {
        const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
        const float soft2 = kGravSoft * kGravSoft;
        const size_t i = pe.i();

        float* __restrict pw = pe.container().w.data();
        float* __restrict pd = pe.container().d.data();
        float* __restrict ph = pe.container().h.data();
        float* __restrict vx = ve.container().x.data();
        float* __restrict vy = ve.container().y.data();
        float* __restrict vz = ve.container().z.data();

        const float dx = sx - pw[i], dy = sy - pd[i], dz = sz - ph[i];
        const float rr = dx * dx + dy * dy + dz * dz;
        const float r2 = rr < soft2 ? soft2 : rr;
        const float k  = gm / (r2 * std::sqrt(r2));

        vx[i] += dx * k * kSimDt;
        vy[i] += dy * k * kSimDt;
        vz[i] += dz * k * kSimDt;

        pw[i] += vx[i] * kSimDt;
        pd[i] += vy[i] * kSimDt;
        ph[i] += vz[i] * kSimDt;
    });
}

// ── V17/V18: можно ли починить ПОЭЛЕМЕНТНУЮ ветку ForEach ───────────────────
// V15 показал, что поэлементная форма не векторизуется (ObjectManager.inl:122,
// код 500). Здесь проверяется, лечится ли это в самом обходе, а не в вызывающем.
//
// Подозрение: SoAElement несёт указатель на SoA-ОБЪЕКТ, поэтому каждое P.w[i]
// внутри лямбды = «загрузить указатель из std::vector, затем индексировать», и,
// раз лямбда туда же пишет, компилятор перечитывает эти указатели каждую итерацию.
//
// V17 повторяет структуру ForEach (цикл снаружи, тело — вызываемый объект на
// каждую сущность), но элемент несёт УЖЕ ПОДНЯТЫЕ указатели __restrict.
// V18 — то же, но вызываемое передаётся как параметр шаблона, ровно как в ForEach,
// чтобы проверить, не в границе вызова ли дело.
struct ElemView {
    float* __restrict pw;
    float* __restrict pd;
    float* __restrict ph;
    float* __restrict vx;
    float* __restrict vy;
    float* __restrict vz;
    int i;
};

void StepHoistedProxy(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEach<Positions, Velocities>(scene, [&s](Positions& P, Velocities& V)
    {
        const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
        const float soft2 = kGravSoft * kGravSoft;
        const int n = static_cast<int>(P.w.size());

        ElemView e{ P.w.data(), P.d.data(), P.h.data(),
                    V.x.data(), V.y.data(), V.z.data(), 0 };

        auto body = [sx, sy, sz, gm, soft2](ElemView e) {
            const float dx = sx - e.pw[e.i], dy = sy - e.pd[e.i], dz = sz - e.ph[e.i];
            const float rr = dx * dx + dy * dy + dz * dz;
            const float r2 = rr < soft2 ? soft2 : rr;
            const float k  = gm / (r2 * std::sqrt(r2));

            e.vx[e.i] += dx * k * kSimDt;
            e.vy[e.i] += dy * k * kSimDt;
            e.vz[e.i] += dz * k * kSimDt;

            e.pw[e.i] += e.vx[e.i] * kSimDt;
            e.pd[e.i] += e.vy[e.i] * kSimDt;
            e.ph[e.i] += e.vz[e.i] * kSimDt;
        };

        for (int i = 0; i < n; ++i) { e.i = i; body(e); }
    });
}

// Цикл, вынесенный в шаблон: вызываемое приходит параметром, как f в ForEach.
template <class Fn>
void DriveElements(const ElemView& base, int n, Fn&& body)
{
    ElemView e = base;
    for (int i = 0; i < n; ++i) { e.i = i; body(e); }
}

void StepHoistedProxyTemplate(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEach<Positions, Velocities>(scene, [&s](Positions& P, Velocities& V)
    {
        const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
        const float soft2 = kGravSoft * kGravSoft;
        const int n = static_cast<int>(P.w.size());

        const ElemView base{ P.w.data(), P.d.data(), P.h.data(),
                             V.x.data(), V.y.data(), V.z.data(), 0 };

        DriveElements(base, n, [sx, sy, sz, gm, soft2](const ElemView& e) {
            const float dx = sx - e.pw[e.i], dy = sy - e.pd[e.i], dz = sz - e.ph[e.i];
            const float rr = dx * dx + dy * dy + dz * dz;
            const float r2 = rr < soft2 ? soft2 : rr;
            const float k  = gm / (r2 * std::sqrt(r2));

            e.vx[e.i] += dx * k * kSimDt;
            e.vy[e.i] += dy * k * kSimDt;
            e.vz[e.i] += dz * k * kSimDt;

            e.pw[e.i] += e.vx[e.i] * kSimDt;
            e.pd[e.i] += e.vy[e.i] * kSimDt;
            e.ph[e.i] += e.vz[e.i] * kSimDt;
        });
    });
}

// ── V19/V20: последняя форма поэлементного обхода ───────────────────────────
// V17/V18 всё ещё не векторизуются, но код сменился (500 -> 1305/1203): значит
// мешает уже не поток управления, а сам элемент-объект — его копируют и мутируют
// (e.i = i) на каждой итерации. Убираем объект: тело получает ГОЛЫЙ ИНДЕКС, а
// указатели подняты до цикла и живут в захвате.
//
// V19 — цикл на месте, тело вызывается как объект-функция.
// V20 — цикл вынесен в шаблон-драйвер (как ForEach), тело принимает только индекс.
// Если хоть одна форма векторизуется, поэлементный API движка можно сделать
// пригодным; если нет — вывод «поэлементная ветка невекторизуема по устройству».
void StepIndexCallable(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEach<Positions, Velocities>(scene, [&s](Positions& P, Velocities& V)
    {
        const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
        const float soft2 = kGravSoft * kGravSoft;
        const int n = static_cast<int>(P.w.size());

        float* __restrict pw = P.w.data();
        float* __restrict pd = P.d.data();
        float* __restrict ph = P.h.data();
        float* __restrict vx = V.x.data();
        float* __restrict vy = V.y.data();
        float* __restrict vz = V.z.data();

        auto body = [=](int i) {
            const float dx = sx - pw[i], dy = sy - pd[i], dz = sz - ph[i];
            const float rr = dx * dx + dy * dy + dz * dz;
            const float r2 = rr < soft2 ? soft2 : rr;
            const float k  = gm / (r2 * std::sqrt(r2));

            vx[i] += dx * k * kSimDt;
            vy[i] += dy * k * kSimDt;
            vz[i] += dz * k * kSimDt;

            pw[i] += vx[i] * kSimDt;
            pd[i] += vy[i] * kSimDt;
            ph[i] += vz[i] * kSimDt;
        };

        for (int i = 0; i < n; ++i) body(i);
    });
}

// Драйвер: цикл принадлежит «движку», тело приходит параметром шаблона.
template <class Fn>
void DriveIndices(int n, Fn&& body)
{
    for (int i = 0; i < n; ++i) body(i);
}

void StepIndexDriver(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEach<Positions, Velocities>(scene, [&s](Positions& P, Velocities& V)
    {
        const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
        const float soft2 = kGravSoft * kGravSoft;
        const int n = static_cast<int>(P.w.size());

        float* __restrict pw = P.w.data();
        float* __restrict pd = P.d.data();
        float* __restrict ph = P.h.data();
        float* __restrict vx = V.x.data();
        float* __restrict vy = V.y.data();
        float* __restrict vz = V.z.data();

        DriveIndices(n, [=](int i) {
            const float dx = sx - pw[i], dy = sy - pd[i], dz = sz - ph[i];
            const float rr = dx * dx + dy * dy + dz * dz;
            const float r2 = rr < soft2 ? soft2 : rr;
            const float k  = gm / (r2 * std::sqrt(r2));

            vx[i] += dx * k * kSimDt;
            vy[i] += dy * k * kSimDt;
            vz[i] += dz * k * kSimDt;

            pw[i] += vx[i] * kSimDt;
            pd[i] += vy[i] * kSimDt;
            ph[i] += vz[i] * kSimDt;
        });
    });
}

// ── V21/V22: то же через ForEachArchetype ───────────────────────────────────
// ForEach на всех-SoA компонентах фактически вырождается в ForEachArchetype: обе
// формы отдают колонки целиком раз на архетип, отличие лишь в том, ЧТО приходит в
// лямбду — сами SoA-объекты (ForEach) или указатели на ComponentArray
// (ForEachArchetype). Со стороны вызова это неочевидно, поэтому проверяем, что
// вердикт векторизации не зависит от выбора формы.
//
// Тело — ровно V19: указатели подняты над циклом, телу отдаётся голый индекс.
// V21 — цикл на месте, V22 — цикл в шаблоне-драйвере (как V20).
void StepArchetypeIndexCallable(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEachArchetype<Positions, Velocities>(scene,
        [&s](ComponentArray<Positions, void>* pa, ComponentArray<Velocities, void>* va)
    {
        Positions&  P = pa->data;
        Velocities& V = va->data;

        const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
        const float soft2 = kGravSoft * kGravSoft;
        const int n = static_cast<int>(P.w.size());

        float* __restrict pw = P.w.data();
        float* __restrict pd = P.d.data();
        float* __restrict ph = P.h.data();
        float* __restrict vx = V.x.data();
        float* __restrict vy = V.y.data();
        float* __restrict vz = V.z.data();

        auto body = [=](int i) {
            const float dx = sx - pw[i], dy = sy - pd[i], dz = sz - ph[i];
            const float rr = dx * dx + dy * dy + dz * dz;
            const float r2 = rr < soft2 ? soft2 : rr;
            const float k  = gm / (r2 * std::sqrt(r2));

            vx[i] += dx * k * kSimDt;
            vy[i] += dy * k * kSimDt;
            vz[i] += dz * k * kSimDt;

            pw[i] += vx[i] * kSimDt;
            pd[i] += vy[i] * kSimDt;
            ph[i] += vz[i] * kSimDt;
        };

        for (int i = 0; i < n; ++i) body(i);
    });
}

void StepArchetypeIndexDriver(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEachArchetype<Positions, Velocities>(scene,
        [&s](ComponentArray<Positions, void>* pa, ComponentArray<Velocities, void>* va)
    {
        Positions&  P = pa->data;
        Velocities& V = va->data;

        const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
        const float soft2 = kGravSoft * kGravSoft;
        const int n = static_cast<int>(P.w.size());

        float* __restrict pw = P.w.data();
        float* __restrict pd = P.d.data();
        float* __restrict ph = P.h.data();
        float* __restrict vx = V.x.data();
        float* __restrict vy = V.y.data();
        float* __restrict vz = V.z.data();

        DriveIndices(n, [=](int i) {
            const float dx = sx - pw[i], dy = sy - pd[i], dz = sz - ph[i];
            const float rr = dx * dx + dy * dy + dz * dz;
            const float r2 = rr < soft2 ? soft2 : rr;
            const float k  = gm / (r2 * std::sqrt(r2));

            vx[i] += dx * k * kSimDt;
            vy[i] += dy * k * kSimDt;
            vz[i] += dz * k * kSimDt;

            pw[i] += vx[i] * kSimDt;
            pd[i] += vy[i] * kSimDt;
            ph[i] += vz[i] * kSimDt;
        });
    });
}

// ── V23/V24: новая форма ForEachArchetype — колонки + entities ──────────────
// V23 проверяет, что сама перегрузка ничего не стоит: тело то же, что в V19.
// V24 — то, ради чего форма и заводилась: по ходу горячего прохода надо отметить
// сущности-кандидаты и получить их id. Отметка пишется БЕЗ ветвления (маска в
// байт), а сборка id вынесена во второй, дешёвый скалярный проход: сжатие
// (idx[cnt] = ...; cnt += hit) внутри горячего цикла векторизовать нельзя.
std::vector<uint8_t> g_hit;
std::vector<Entity>  g_hits;

void StepEntityListPlain(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEachArchetype<Positions, Velocities>(scene,
        [&s](ComponentArray<Positions, void>* pa, ComponentArray<Velocities, void>* va,
             const std::vector<Entity>& ents)
    {
        Positions&  P = pa->data;
        Velocities& V = va->data;
        const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
        const float soft2 = kGravSoft * kGravSoft;
        const int n = static_cast<int>(ents.size());

        float* __restrict pw = P.w.data();
        float* __restrict pd = P.d.data();
        float* __restrict ph = P.h.data();
        float* __restrict vx = V.x.data();
        float* __restrict vy = V.y.data();
        float* __restrict vz = V.z.data();

        for (int i = 0; i < n; ++i) {
            const float dx = sx - pw[i], dy = sy - pd[i], dz = sz - ph[i];
            const float rr = dx * dx + dy * dy + dz * dz;
            const float r2 = rr < soft2 ? soft2 : rr;
            const float k  = gm / (r2 * std::sqrt(r2));

            vx[i] += dx * k * kSimDt;
            vy[i] += dy * k * kSimDt;
            vz[i] += dz * k * kSimDt;

            pw[i] += vx[i] * kSimDt;
            pd[i] += vy[i] * kSimDt;
            ph[i] += vz[i] * kSimDt;
        }
    });
}

void StepEntityListCollect(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEachArchetype<Positions, Velocities>(scene,
        [&s](ComponentArray<Positions, void>* pa, ComponentArray<Velocities, void>* va,
             const std::vector<Entity>& ents)
    {
        Positions&  P = pa->data;
        Velocities& V = va->data;
        const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
        const float soft2 = kGravSoft * kGravSoft;
        const float box2  = 400.0f;   // условный радиус интереса, как рамка в игре
        const int n = static_cast<int>(ents.size());
        if (static_cast<int>(g_hit.size()) < n) g_hit.resize(n);

        float*   __restrict pw  = P.w.data();
        float*   __restrict pd  = P.d.data();
        float*   __restrict ph  = P.h.data();
        float*   __restrict vx  = V.x.data();
        float*   __restrict vy  = V.y.data();
        float*   __restrict vz  = V.z.data();
        uint8_t* __restrict hit = g_hit.data();

        // Горячий проход: та же арифметика + отметка кандидата значением, без ветки.
        for (int i = 0; i < n; ++i) {
            const float dx = sx - pw[i], dy = sy - pd[i], dz = sz - ph[i];
            const float rr = dx * dx + dy * dy + dz * dz;
            const float r2 = rr < soft2 ? soft2 : rr;
            const float k  = gm / (r2 * std::sqrt(r2));

            hit[i] = rr < box2 ? uint8_t(1) : uint8_t(0);

            vx[i] += dx * k * kSimDt;
            vy[i] += dy * k * kSimDt;
            vz[i] += dz * k * kSimDt;

            pw[i] += vx[i] * kSimDt;
            pd[i] += vy[i] * kSimDt;
            ph[i] += vz[i] * kSimDt;
        }

        // Сборка id: отдельный проход, кандидатов единицы. Здесь и нужен entities.
        g_hits.clear();
        for (int i = 0; i < n; ++i)
            if (hit[i]) g_hits.push_back(ents[i]);
    });
}

// ── V25/V26: отметка кандидата шириной во float / в int ─────────────────────
// V24 споткнулся на uint8_t: однобайтовая запись посреди 32-битной арифметики
// (код 1100). Ширина элемента маски должна совпадать с шириной данных — пробуем
// float и int32.
std::vector<float>    g_hitf;
std::vector<uint32_t> g_hitu;

void StepCollectFloatMask(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEachArchetype<Positions, Velocities>(scene,
        [&s](ComponentArray<Positions, void>* pa, ComponentArray<Velocities, void>* va,
             const std::vector<Entity>& ents)
    {
        Positions&  P = pa->data;
        Velocities& V = va->data;
        const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
        const float soft2 = kGravSoft * kGravSoft;
        const float box2  = 400.0f;
        const int n = static_cast<int>(ents.size());
        if (static_cast<int>(g_hitf.size()) < n) g_hitf.resize(n);

        float* __restrict pw  = P.w.data();
        float* __restrict pd  = P.d.data();
        float* __restrict ph  = P.h.data();
        float* __restrict vx  = V.x.data();
        float* __restrict vy  = V.y.data();
        float* __restrict vz  = V.z.data();
        float* __restrict hit = g_hitf.data();

        for (int i = 0; i < n; ++i) {
            const float dx = sx - pw[i], dy = sy - pd[i], dz = sz - ph[i];
            const float rr = dx * dx + dy * dy + dz * dz;
            const float r2 = rr < soft2 ? soft2 : rr;
            const float k  = gm / (r2 * std::sqrt(r2));

            hit[i] = rr < box2 ? 1.0f : 0.0f;

            vx[i] += dx * k * kSimDt;
            vy[i] += dy * k * kSimDt;
            vz[i] += dz * k * kSimDt;

            pw[i] += vx[i] * kSimDt;
            pd[i] += vy[i] * kSimDt;
            ph[i] += vz[i] * kSimDt;
        }

        g_hits.clear();
        for (int i = 0; i < n; ++i)
            if (hit[i] != 0.0f) g_hits.push_back(ents[i]);
    });
}

void StepCollectIntMask(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEachArchetype<Positions, Velocities>(scene,
        [&s](ComponentArray<Positions, void>* pa, ComponentArray<Velocities, void>* va,
             const std::vector<Entity>& ents)
    {
        Positions&  P = pa->data;
        Velocities& V = va->data;
        const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
        const float soft2 = kGravSoft * kGravSoft;
        const float box2  = 400.0f;
        const int n = static_cast<int>(ents.size());
        if (static_cast<int>(g_hitu.size()) < n) g_hitu.resize(n);

        float*    __restrict pw  = P.w.data();
        float*    __restrict pd  = P.d.data();
        float*    __restrict ph  = P.h.data();
        float*    __restrict vx  = V.x.data();
        float*    __restrict vy  = V.y.data();
        float*    __restrict vz  = V.z.data();
        uint32_t* __restrict hit = g_hitu.data();

        for (int i = 0; i < n; ++i) {
            const float dx = sx - pw[i], dy = sy - pd[i], dz = sz - ph[i];
            const float rr = dx * dx + dy * dy + dz * dz;
            const float r2 = rr < soft2 ? soft2 : rr;
            const float k  = gm / (r2 * std::sqrt(r2));

            hit[i] = rr < box2 ? 1u : 0u;

            vx[i] += dx * k * kSimDt;
            vy[i] += dy * k * kSimDt;
            vz[i] += dz * k * kSimDt;

            pw[i] += vx[i] * kSimDt;
            pd[i] += vy[i] * kSimDt;
            ph[i] += vz[i] * kSimDt;
        }

        g_hits.clear();
        for (int i = 0; i < n; ++i)
            if (hit[i]) g_hits.push_back(ents[i]);
    });
}

// ── V27: кандидаты через БЕЗУСЛОВНУЮ запись величины ────────────────────────
// V24-V26 показали: условный флаг (ternary в отдельный массив) ломает векторизацию
// при любой ширине элемента. Значит из горячего цикла надо убрать не ширину, а
// САМО УСЛОВИЕ: пишем безусловно уже посчитанный квадрат расстояния, а сравнение
// и сборку id делает второй проход — он дешёвый и скалярный по своей природе
// (сжатие в список всё равно не векторизуется).
std::vector<float> g_rr;

void StepCollectViaValue(ObjectManager& om, SceneData* scene, const Src& s)
{
    om.ForEachArchetype<Positions, Velocities>(scene,
        [&s](ComponentArray<Positions, void>* pa, ComponentArray<Velocities, void>* va,
             const std::vector<Entity>& ents)
    {
        Positions&  P = pa->data;
        Velocities& V = va->data;
        const float sx = s.x, sy = s.y, sz = s.z, gm = s.gm;
        const float soft2 = kGravSoft * kGravSoft;
        const float box2  = 400.0f;
        const int n = static_cast<int>(ents.size());
        if (static_cast<int>(g_rr.size()) < n) g_rr.resize(n);

        float* __restrict pw = P.w.data();
        float* __restrict pd = P.d.data();
        float* __restrict ph = P.h.data();
        float* __restrict vx = V.x.data();
        float* __restrict vy = V.y.data();
        float* __restrict vz = V.z.data();
        float* __restrict rr2 = g_rr.data();

        for (int i = 0; i < n; ++i) {
            const float dx = sx - pw[i], dy = sy - pd[i], dz = sz - ph[i];
            const float rr = dx * dx + dy * dy + dz * dz;
            const float r2 = rr < soft2 ? soft2 : rr;
            const float k  = gm / (r2 * std::sqrt(r2));

            rr2[i] = rr;   // безусловно: обычная запись, условия в цикле нет

            vx[i] += dx * k * kSimDt;
            vy[i] += dy * k * kSimDt;
            vz[i] += dz * k * kSimDt;

            pw[i] += vx[i] * kSimDt;
            pd[i] += vy[i] * kSimDt;
            ph[i] += vz[i] * kSimDt;
        }

        g_hits.clear();
        for (int i = 0; i < n; ++i)
            if (rr2[i] < box2) g_hits.push_back(ents[i]);
    });
}

// ── Снимок и восстановление колонок ─────────────────────────────────────────
// Каждый вариант обязан стартовать с одних и тех же данных, иначе орбиты
// разъедутся и варианты будут считать разное.
struct Snapshot {
    std::vector<float> w, d, h, vx, vy, vz;

    void Take(ObjectManager& om, SceneData* scene) {
        om.ForEach<Positions, Velocities>(scene, [this](Positions& P, Velocities& V) {
            w = P.w; d = P.d; h = P.h; vx = V.x; vy = V.y; vz = V.z;
        });
    }
    void Restore(ObjectManager& om, SceneData* scene) const {
        om.ForEach<Positions, Velocities>(scene, [this](Positions& P, Velocities& V) {
            P.w = w; P.d = d; P.h = h; V.x = vx; V.y = vy; V.z = vz;
        });
    }
};

double Checksum(ObjectManager& om, SceneData* scene)
{
    double s = 0.0;
    om.ForEach<Positions, Velocities>(scene, [&s](Positions& P, Velocities& V) {
        for (size_t i = 0; i < P.w.size(); i += 4096) s += P.w[i] + V.y[i];
    });
    return s;
}

template <class Fn>
void Bench(const char* name, ObjectManager& om, SceneData* scene, const Snapshot& snap, Fn&& step)
{
    double best = 1e9, sum = 0.0, checksum = 0.0;
    for (int k = 0; k < kRepeats; ++k) {
        snap.Restore(om, scene);
        const auto t0 = Clock::now();
        step();
        const double ms = MsSince(t0);
        best = std::min(best, ms);
        sum += ms;
        checksum = Checksum(om, scene);
    }
    std::printf("  %-34s best %7.3f ms   avg %7.3f ms   (checksum %.3f)\n",
        name, best, sum / kRepeats, checksum);
    std::fflush(stdout);
}

} // namespace

int main(int, char**)
{
    std::printf("\n=== GravityVecProbe: N = %zu, %d прогонов на вариант ===\n", N, kRepeats);
    std::printf("    Обход - настоящий ObjectManager::ForEach<Positions, Velocities>.\n");
    std::printf("    Вердикт векторизации - в отчёте сборки (/Qvec-report:2).\n\n");

    ObjectManager om;
    om.CreateScene("probe");
    om.SetActiveScene("probe");

    // Сущности создаём штатным путём: один архетип {Positions, Velocities}, кольцо
    // радиусов 50..350 с круговой скоростью sqrt(GM/r) - как раздаёт scene_gen.py.
    const auto t_build = Clock::now();
    for (size_t i = 0; i < N; ++i) {
        const float t = static_cast<float>(i) * 0.0001f;
        const float r = 50.0f + static_cast<float>(i % 300);
        const float v = std::sqrt(kGM / r);

        PositionProxy16 p{};                       // единичная матрица; трансляция - w/d/h
        p.w = r * std::cos(t);
        p.d = static_cast<float>(i % 17) - 8.0f;
        p.h = r * std::sin(t);

        om.CreateEntity("probe", p, VelocityProxy{ -v * std::sin(t), 0.0f, v * std::cos(t) });
    }
    std::printf("  build: %.0f ms (%zu сущностей)\n\n", MsSince(t_build), N);

    SceneData* scene = om.GetActiveScene();

    Snapshot snap;
    snap.Take(om, scene);

    const Src src{ 0.0f, 0.0f, 0.0f, kGM };
    const std::vector<Src> sources{ src };

    Bench("V0 game shape (vector + continue)", om, scene, snap, [&] { StepGameShape(om, scene, sources); });
    Bench("V1 hoisted source, branch kept",    om, scene, snap, [&] { StepHoistedBranch(om, scene, src); });
    Bench("V2 branchless (max instead of if)", om, scene, snap, [&] { StepBranchless(om, scene, src); });
    Bench("V3 branchless + pragma ivdep",      om, scene, snap, [&] { StepBranchlessIvdep(om, scene, src); });
    Bench("V4 branchless + __restrict ptrs",   om, scene, snap, [&] { StepRestrict(om, scene, src); });
    Bench("V5 V4 + int index",                 om, scene, snap, [&] { StepRestrictInt(om, scene, src); });
    Bench("V6 branchless + int index",         om, scene, snap, [&] { StepBranchlessInt(om, scene, src); });
    Bench("V7 no sqrt, no div",                om, scene, snap, [&] { StepNoSqrtNoDiv(om, scene, src); });
    Bench("V8 sqrt, no div",                   om, scene, snap, [&] { StepSqrtNoDiv(om, scene, src); });
    Bench("V9 div, no sqrt",                   om, scene, snap, [&] { StepDivNoSqrt(om, scene, src); });
    Bench("V10 rsqrt Newton (no div at all)",  om, scene, snap, [&] { StepRsqrtNewton(om, scene, src); });
    Bench("V11 control: bare division",        om, scene, snap, [&] { StepDivOnly(om, scene, src); });
    Bench("V12 three passes + scratch",        om, scene, snap, [&] { StepThreePass(om, scene, src); });
    Bench("V13 ternary clamp + restrict",      om, scene, snap, [&] { StepTernaryClamp(om, scene, src); });
    Bench("V14 ternary clamp, no restrict",    om, scene, snap, [&] { StepTernaryNoRestrict(om, scene, src); });
    Bench("V15 SoAElement per entity",         om, scene, snap, [&] { StepSoAElement(om, scene, src); });
    Bench("V16 SoAElement + restrict ptrs",    om, scene, snap, [&] { StepSoAElementImplicit(om, scene, src); });
    Bench("V17 per-elem, hoisted restrict",    om, scene, snap, [&] { StepHoistedProxy(om, scene, src); });
    Bench("V18 per-elem via template driver",  om, scene, snap, [&] { StepHoistedProxyTemplate(om, scene, src); });
    Bench("V19 per-elem, index callable",      om, scene, snap, [&] { StepIndexCallable(om, scene, src); });
    Bench("V20 index callable via driver",     om, scene, snap, [&] { StepIndexDriver(om, scene, src); });
    Bench("V21 ForEachArchetype + index",      om, scene, snap, [&] { StepArchetypeIndexCallable(om, scene, src); });
    Bench("V22 ForEachArchetype + driver",     om, scene, snap, [&] { StepArchetypeIndexDriver(om, scene, src); });
    Bench("V23 ForEachArch + entities"     ,    om, scene, snap, [&] { StepEntityListPlain(om, scene, src); });
    Bench("V24 + hit mask & id collect",       om, scene, snap, [&] { StepEntityListCollect(om, scene, src); });
    Bench("V25 float mask & id collect",       om, scene, snap, [&] { StepCollectFloatMask(om, scene, src); });
    Bench("V26 uint32 mask & id collect",      om, scene, snap, [&] { StepCollectIntMask(om, scene, src); });
    Bench("V27 value store & id collect",      om, scene, snap, [&] { StepCollectViaValue(om, scene, src); });

    std::printf("\n");
    return 0;
}
