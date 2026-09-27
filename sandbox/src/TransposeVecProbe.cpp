// ============================================================================
//  Sandbox: нужны ли интринсики в TransformDataModule.
//
//  Зачем. В модуле два места с ручным SIMD: AVX-транспонирование 8x8 в
//  TransposeSoAToMatrices (SoA колонок Positions -> AoS матриц в transfer-буфере)
//  и SSE-умножение MulMat4InPlace. Вопрос: это задача, которую компилятор не берёт
//  в принципе, или она не взялась ИМЕННО в той форме, в какой написана?
//
//  Методика — как в GravityVecProbe: лесенка вариантов, каждый снимает одно
//  подозрение, вердикт векторизации читается в отчёте сборки (/Qvec-report:2),
//  здесь печатается только время.
//
//    T0  скаляр, как fallback модуля: индекс size_t, доступ через src[s][e]
//    T1  то же, но указатели подняты в локали, __restrict, индекс int
//    T2  внешний цикл по потокам (непрерывное чтение, запись с шагом 64 байта)
//    T3  блок 8 сущностей через маленький буфер в L1 — БЕЗ интринсиков
//    T4  AVX 8x8, ровно как сейчас в модуле
//    T5  16 memcpy по колонкам: транспонирования нет вовсе (потолок полосы)
//
//  M0/M1 — то же для умножения матриц: SSE-версия модуля против простого C.
//
//  Данные — 16 std::vector<float>, ровно как колонки Positions; назначение —
//  обычная память (transfer-буфер по замерам ведёт себя как кэшируемая).
// ============================================================================
#include "PCH.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>
#include <immintrin.h>

namespace {

constexpr size_t N        = 800'000;   // столько же, сколько в тяжёлой сцене
constexpr int    kRepeats = 15;

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// ── T0: как fallback модуля ─────────────────────────────────────────────────
void T0_Scalar(const float* const src[16], float* dst, size_t n)
{
    for (size_t e = 0; e < n; ++e) {
        float* m = dst + e * 16;
        for (size_t s = 0; s < 16; ++s)
            m[s] = src[s][e];
    }
}

// ── T1: форма, которую любит автовекторизатор ───────────────────────────────
// Уроки GravityVecProbe: указатели в локалях, __restrict, индекс int.
void T1_ScalarRestrict(const float* const src[16], float* dst, int n)
{
    const float* __restrict s0  = src[0];  const float* __restrict s1  = src[1];
    const float* __restrict s2  = src[2];  const float* __restrict s3  = src[3];
    const float* __restrict s4  = src[4];  const float* __restrict s5  = src[5];
    const float* __restrict s6  = src[6];  const float* __restrict s7  = src[7];
    const float* __restrict s8  = src[8];  const float* __restrict s9  = src[9];
    const float* __restrict s10 = src[10]; const float* __restrict s11 = src[11];
    const float* __restrict s12 = src[12]; const float* __restrict s13 = src[13];
    const float* __restrict s14 = src[14]; const float* __restrict s15 = src[15];
    float* __restrict out = dst;

    for (int e = 0; e < n; ++e) {
        float* __restrict m = out + e * 16;
        m[0]  = s0[e];  m[1]  = s1[e];  m[2]  = s2[e];  m[3]  = s3[e];
        m[4]  = s4[e];  m[5]  = s5[e];  m[6]  = s6[e];  m[7]  = s7[e];
        m[8]  = s8[e];  m[9]  = s9[e];  m[10] = s10[e]; m[11] = s11[e];
        m[12] = s12[e]; m[13] = s13[e]; m[14] = s14[e]; m[15] = s15[e];
    }
}

// ── T2: поток снаружи ───────────────────────────────────────────────────────
// Чтение непрерывное, зато запись с шагом 16 float. Проверяем, не возьмёт ли
// компилятор такую форму (нужен scatter, которого в AVX2 нет).
void T2_PerStream(const float* const src[16], float* dst, int n)
{
    for (int s = 0; s < 16; ++s) {
        const float* __restrict in = src[s];
        float* __restrict out = dst + s;
        for (int e = 0; e < n; ++e)
            out[e * 16] = in[e];
    }
}

// ── T3: блок 8 сущностей через L1, без интринсиков ──────────────────────────
// Развязка задачи на два цикла, каждый из которых компилятору по силам:
// непрерывное чтение колонок в крошечный буфер, потом непрерывная запись матриц.
void T3_Blocked(const float* const src[16], float* dst, int n)
{
    int e = 0;
    for (; e + 8 <= n; e += 8) {
        float block[16][8];
        for (int s = 0; s < 16; ++s) {
            const float* __restrict in = src[s] + e;
            for (int k = 0; k < 8; ++k)
                block[s][k] = in[k];
        }
        float* __restrict out = dst + static_cast<size_t>(e) * 16;
        for (int m = 0; m < 8; ++m)
            for (int s = 0; s < 16; ++s)
                out[m * 16 + s] = block[s][m];
    }
    for (; e < n; ++e) {
        float* m = dst + static_cast<size_t>(e) * 16;
        for (int s = 0; s < 16; ++s)
            m[s] = src[s][e];
    }
}

// ── T4: AVX 8x8, ровно как в модуле ─────────────────────────────────────────
inline void Transpose8x8(__m256 r[8])
{
    __m256 t0 = _mm256_unpacklo_ps(r[0], r[1]);
    __m256 t1 = _mm256_unpackhi_ps(r[0], r[1]);
    __m256 t2 = _mm256_unpacklo_ps(r[2], r[3]);
    __m256 t3 = _mm256_unpackhi_ps(r[2], r[3]);
    __m256 t4 = _mm256_unpacklo_ps(r[4], r[5]);
    __m256 t5 = _mm256_unpackhi_ps(r[4], r[5]);
    __m256 t6 = _mm256_unpacklo_ps(r[6], r[7]);
    __m256 t7 = _mm256_unpackhi_ps(r[6], r[7]);

    __m256 u0 = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(1, 0, 1, 0));
    __m256 u1 = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(3, 2, 3, 2));
    __m256 u2 = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(1, 0, 1, 0));
    __m256 u3 = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(3, 2, 3, 2));
    __m256 u4 = _mm256_shuffle_ps(t4, t6, _MM_SHUFFLE(1, 0, 1, 0));
    __m256 u5 = _mm256_shuffle_ps(t4, t6, _MM_SHUFFLE(3, 2, 3, 2));
    __m256 u6 = _mm256_shuffle_ps(t5, t7, _MM_SHUFFLE(1, 0, 1, 0));
    __m256 u7 = _mm256_shuffle_ps(t5, t7, _MM_SHUFFLE(3, 2, 3, 2));

    r[0] = _mm256_permute2f128_ps(u0, u4, 0x20);
    r[1] = _mm256_permute2f128_ps(u1, u5, 0x20);
    r[2] = _mm256_permute2f128_ps(u2, u6, 0x20);
    r[3] = _mm256_permute2f128_ps(u3, u7, 0x20);
    r[4] = _mm256_permute2f128_ps(u0, u4, 0x31);
    r[5] = _mm256_permute2f128_ps(u1, u5, 0x31);
    r[6] = _mm256_permute2f128_ps(u2, u6, 0x31);
    r[7] = _mm256_permute2f128_ps(u3, u7, 0x31);
}

void T4_Avx(const float* const src[16], float* dst, size_t n)
{
    size_t e = 0;
    for (; e + 8 <= n; e += 8) {
        __m256 lo[8], hi[8];
        for (int s = 0; s < 8; ++s) {
            lo[s] = _mm256_loadu_ps(src[s] + e);
            hi[s] = _mm256_loadu_ps(src[s + 8] + e);
        }
        Transpose8x8(lo);
        Transpose8x8(hi);

        float* out = dst + e * 16;
        for (int m = 0; m < 8; ++m) {
            _mm256_storeu_ps(out + m * 16,     lo[m]);
            _mm256_storeu_ps(out + m * 16 + 8, hi[m]);
        }
    }
    for (; e < n; ++e) {
        float* m = dst + e * 16;
        for (size_t s = 0; s < 16; ++s)
            m[s] = src[s][e];
    }
}

// ── T5: потолок полосы ──────────────────────────────────────────────────────
// Столько же прочитанных и записанных байт, но без перестановки: колонки едут
// подряд. Это то, что осталось бы, читай GPU колонки, а не матрицы.
void T5_ColumnDump(const float* const src[16], float* dst, size_t n)
{
    for (size_t s = 0; s < 16; ++s)
        std::memcpy(dst + s * n, src[s], n * sizeof(float));
}

// ── Умножение матриц ────────────────────────────────────────────────────────
// M0 — как в модуле: SSE, in-place, столбцы сняты до первой записи.
inline void M0_MulSse(float* lhs, const float* rhs)
{
    const __m128 c0 = _mm_loadu_ps(lhs + 0);
    const __m128 c1 = _mm_loadu_ps(lhs + 4);
    const __m128 c2 = _mm_loadu_ps(lhs + 8);
    const __m128 c3 = _mm_loadu_ps(lhs + 12);

    for (int j = 0; j < 4; ++j) {
        const float* b = rhs + j * 4;
        __m128 col =          _mm_mul_ps(c0, _mm_set1_ps(b[0]));
        col = _mm_add_ps(col, _mm_mul_ps(c1, _mm_set1_ps(b[1])));
        col = _mm_add_ps(col, _mm_mul_ps(c2, _mm_set1_ps(b[2])));
        col = _mm_add_ps(col, _mm_mul_ps(c3, _mm_set1_ps(b[3])));
        _mm_storeu_ps(lhs + j * 4, col);
    }
}

// M1 — простой C в отдельный выход: алиасинга нет, границы константны.
inline void M1_MulPlain(const float* a, const float* b, float* out)
{
    for (int j = 0; j < 4; ++j) {
        const float b0 = b[j * 4 + 0], b1 = b[j * 4 + 1];
        const float b2 = b[j * 4 + 2], b3 = b[j * 4 + 3];
        for (int i = 0; i < 4; ++i)
            out[j * 4 + i] = a[i] * b0 + a[4 + i] * b1 + a[8 + i] * b2 + a[12 + i] * b3;
    }
}

// ── обвязка ─────────────────────────────────────────────────────────────────
double Checksum(const float* p, size_t count)
{
    double acc = 0.0;
    for (size_t i = 0; i < count; i += 997) acc += p[i];
    return acc;
}

template <class Fn>
void Bench(const char* name, float* dst, size_t floats, Fn&& fn)
{
    double best = 1e30, sum = 0.0;
    for (int r = 0; r < kRepeats; ++r) {
        std::memset(dst, 0, floats * sizeof(float));
        const auto t = Clock::now();
        fn();
        const double ms = MsSince(t);
        best = ms < best ? ms : best;
        sum += ms;
    }
    const double gb = static_cast<double>(floats) * sizeof(float) * 2.0 / (1 << 30);
    std::printf("  %-36s best %7.3f ms   avg %7.3f ms   %5.1f GB/s   (checksum %.3f)\n",
        name, best, sum / kRepeats, gb / (best / 1000.0), Checksum(dst, floats));
    std::fflush(stdout);
}

} // namespace

int main(int, char**)
{
    std::printf("\n=== TransposeVecProbe: N = %zu сущностей (%.1f МБ матриц), %d прогонов ===\n",
        N, N * 64.0 / (1 << 20), kRepeats);
    std::printf("    AVX доступен: %s.  Вердикт векторизации - в отчёте сборки (/Qvec-report:2).\n\n",
        SDL_HasAVX() ? "да" : "нет");

    std::vector<std::vector<float>> cols(16);
    const float* src[16];
    for (int s = 0; s < 16; ++s) {
        cols[s].resize(N);
        for (size_t e = 0; e < N; ++e)
            cols[s][e] = static_cast<float>(s) + static_cast<float>(e) * 1e-6f;
        src[s] = cols[s].data();
    }

    std::vector<float> dst(N * 16);
    float* out = dst.data();
    const int n_int = static_cast<int>(N);

    Bench("T0 scalar (fallback модуля)",      out, dst.size(), [&] { T0_Scalar(src, out, N); });
    Bench("T1 scalar + restrict + int",       out, dst.size(), [&] { T1_ScalarRestrict(src, out, n_int); });
    Bench("T2 поток снаружи (scatter)",       out, dst.size(), [&] { T2_PerStream(src, out, n_int); });
    Bench("T3 блок 8 через L1, без SIMD",     out, dst.size(), [&] { T3_Blocked(src, out, n_int); });
    Bench("T4 AVX 8x8 (как в модуле)",        out, dst.size(), [&] { T4_Avx(src, out, N); });
    Bench("T5 dump колонок (без перестановки)", out, dst.size(), [&] { T5_ColumnDump(src, out, N); });

    // Умножение: столько же матриц, сколько сущностей с родителем в тяжёлой сцене.
    constexpr size_t kMuls = 200'000;
    std::vector<float> lhs(kMuls * 16), rhs(kMuls * 16), res(kMuls * 16);
    for (size_t i = 0; i < lhs.size(); ++i) {
        lhs[i] = 1.0f + static_cast<float>(i % 7) * 0.25f;
        rhs[i] = 0.5f + static_cast<float>(i % 5) * 0.125f;
    }

    std::printf("\n  умножение матриц, %zu шт:\n", kMuls);
    Bench("M0 SSE in-place (как в модуле)", res.data(), res.size(), [&] {
        for (size_t i = 0; i < kMuls; ++i) {
            float w[16];
            std::memcpy(w, &lhs[i * 16], sizeof(w));
            M0_MulSse(w, &rhs[i * 16]);
            std::memcpy(&res[i * 16], w, sizeof(w));
        }
    });
    Bench("M1 простой C в отдельный выход", res.data(), res.size(), [&] {
        for (size_t i = 0; i < kMuls; ++i)
            M1_MulPlain(&lhs[i * 16], &rhs[i * 16], &res[i * 16]);
    });

    std::printf("\n");
    return 0;
}
