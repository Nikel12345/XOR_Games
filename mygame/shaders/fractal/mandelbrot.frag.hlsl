// 2D-фрактал: множество Мандельброта во ВСЮ видимую область, от поворота камеры не зависит
// (и от позиции тоже: камера — дельта-аккумулятор, всегда в нуле). Экранная точка приходит
// готовым NDC из fractal2d.vert.hlsl; пан/зум/аспект запечены в буфер кадра на CPU
// (mygame/FractalUpdateSet.cpp, сцена scene_mandelbrot).
//
// ГЛУБИНА — ПЕРЕТУРБАЦИЯ: CPU раз в кадр итерирует РЕФЕРЕНС (центр окна) в double и заливает
// орбиту Z_0..Z_{len-1} буфером (MandelbrotOrbitBuffer). Пиксель итерирует только ДЕЛЬТУ от
// референса: для z = Z + dz алгебраически ТОЧНО dz' = (2Z + dz)·dz + dc,
// где dc = смещение пикселя от центра — оно мало́ по построению и живёт в обычном float.
// Абсолютная координата пикселя нигде не материализуется → нет и потери её младших битов
// (прежний шум float с ~17 октав, потом df до ~45). Предел резкости — double референса
// (~50 октав), наводка пана теперь ТАКАЯ ЖЕ (double-центр копит float-дельты камеры — ulp
// прежней float-позиции больше не потолок). Безлимит — big-float референса, шейдер не меняется.
//
// РЕ-БАЗИРОВАНИЕ (Чжуоран): если |z| < |dz| — точка ближе к началу орбиты (Z_0 = 0), чем к
// текущему референсу: δ перепривязывается (dz = z, m = 0). Это же закрывает классические
// «глитчи» перетурбации и разворот на конце короткой орбиты (референс сбежал).
//
// Сэмплеры — ГЛОБАЛКИ MAIN_PASS (биндит пасс, не материал): слот 0 — тень, слот 1 — env.
// Фракталу не нужны, но объявлены и «заякорены» веткой в main(): DXC стрипает неиспользуемые
// ресурсы, а SDL GPU ждёт ПЛОТНЫЕ слоты от 0 — см. тот же приём в skybox.frag.hlsl.

#include "main_pass/pass_targets.hlsl"

[[vk::combinedImageSampler]]
Texture2DArray<float>     u_shadowDepthArray  : register(t0, space2);
[[vk::combinedImageSampler]]
SamplerComparisonState    u_shadowSampler     : register(s0, space2);

[[vk::combinedImageSampler]]
TextureCube  u_envCube    : register(t1, space2);
[[vk::combinedImageSampler]]
SamplerState u_envSampler : register(s1, space2);

// Кадр Мандельброта (пишет апдейтер MandelbrotOrbitBuffer, mygame/FractalUpdateSet.cpp):
// [0] = (len asuint, half_h), [1] = (aspect, 0), [2 + n] — Z_n референс-орбиты.
// Камерного буфера здесь НЕТ намеренно: пан/зум/аспект запечены сюда на CPU, а
// неиспользуемое объявление DXC стрипает → дырявые слоты storage-буферов (SDL ждёт
// плотные) → пайплайн не собирается.
StructuredBuffer<float2> Orbit : register(t2, space2);

// Push-константы sp "Mandelbrot" (fragment слот 0). Раскладка = FractalPushData (FractalUpdateSet.h).
cbuffer FractalParams : register(b0, space3)
{
    float time;       // анимация палитры
    uint  max_iter;   // потолок итераций (адаптив ниже поднимается к нему с зумом)
    float _pad0;
    float _pad1;
};

struct PSInput
{
    float4 sv_pos : SV_Position;
    [[vk::location(0)]] float2 v_ndc : TEXCOORD0;
};

struct PSOutput
{
    MAIN_PASS_TARGETS               // ambient = 0: множество экранным AO не затеняется
};

// Палитра IQ-косинусами: гладкая, циклится по дробному счётчику побега.
float3 Palette(float t)
{
    return 0.5 + 0.5 * cos(6.28318530718 * (t + float3(0.0, 0.33, 0.67)));
}

PSOutput main(PSInput input)
{
    PSOutput o;

    // Зум/аспект — из заголовка кадра (CPU уже применил авто-масштаб и ручной сдвиг окна).
    uint  len    = asuint(Orbit[0].x);   // фактическая длина орбиты (референс мог сбежать)
    float half_h = Orbit[0].y;
    float aspect = Orbit[1].x;


    // Смещение пикселя от центра окна = от референса. Только оно и нужно: мало́ → float.
    float2 dc = input.v_ndc * float2(half_h * aspect, half_h);

    // Детализация от зума: +96 итераций на октаву (dwell у границы растёт быстро),
    // потолок — max_iter из push.
    float octaves = max(0.0, -log2(half_h));
    uint  it      = min(max_iter, 64u + (uint)(96.0 * octaves));

    float3 col = float3(0.0, 0.0, 0.0);   // дефолт: внутренность множества / нет орбиты
    float3 emi = float3(0.0, 0.0, 0.0);

    if (len >= 2)
    {
        float2 dz = float2(0.0, 0.0);
        float  m2 = 0.0;
        uint   m  = 0;   // индекс в орбите; НЕ равен i — ре-базирование сбрасывает m в 0
        uint   i  = 0;
        for (; i < it; ++i) {
            // dz' = (2Z + dz)·dz + dc (комплексно) — точная алгебра, без приближений.
            float2 Z = Orbit[2 + m];
            float2 t = float2(2.0 * Z.x + dz.x, 2.0 * Z.y + dz.y);
            dz = float2(t.x * dz.x - t.y * dz.y, t.x * dz.y + t.y * dz.x) + dc;
            ++m;

            float2 z = Orbit[2 + m] + dz;   // полное значение — только для тестов/окраски
            m2 = dot(z, z);
            if (m2 > 256.0) break;

            // Ре-базирование: точка ближе к Z_0=0, чем к референсу, ИЛИ орбита кончилась.
            // БЕЗ ВЕТКИ (select): на этом драйвере ветвление `if {dz=z; m=0;}` внутри цикла
            // мискомпилируется — m обнулялся, а присваивание dz=z ТЕРЯЛОСЬ, после чего dz²+dc
            // расходится у всех пикселей (равномерный побег ~18 итераций, зелёная заливка).
            // Родня граблям df/precise: логике шейдеров этого GPU с ветками в циклах не верить.
            const bool rebase = (m2 < dot(dz, dz)) || (m >= len - 1);
            dz = rebase ? z : dz;
            m  = rebase ? 0u : m;
        }

        if (i < it) {
            // Снаружи: гладкий (дробный) счётчик побега → палитра. Быстрый побег (далеко
            // от множества) — темно, у границы (большой sn) — яркие полосы.
            float sn = (float)i + 1.0 - log2(0.5 * log2(m2));
            col = Palette(sn * 0.02 + time * 0.02) * saturate(sn * 0.08);
            emi = col * col * 0.6;   // квадрат: блумят только яркие полосы кромки
        }

    }

    o.color    = float4(col, 1.0);
    o.emission = float4(emi, 0.0);
    o.ambient  = float4(0.0, 0.0, 0.0, 0.0);   // своё затенение, экранный AO не применяется

    // ЯКОРЬ t0/t1: ветка никогда не выполняется (sv_pos.x >= 0 во вьюпорте), но компилятор
    // доказать этого не может → глобалки пасса не стрипаются, слоты остаются плотными.
    if (input.sv_pos.x < -1.0) {
        o.color.r += u_shadowDepthArray.SampleCmpLevelZero(u_shadowSampler, float3(0.0, 0.0, 0.0), 0.0);
        o.color.g += u_envCube.Sample(u_envSampler, float3(0.0, 1.0, 0.0)).g;
    }

    return o;
}
