// 3D-фрактал: губка Менгера рэймарчем в ПЕРЕНОРМИРОВАННОМ КАДРЕ — «4-я координата» позиции
// (непрерывный масштаб s камеры) вынесена из float-координат: CPU (mygame/FractalBackground.cpp)
// держит double-позицию в юнитах корня и каждый тик стейтлесс выводит кадр якорной ячейки
// глубины d(s). Шейдер всегда работает при масштабе ~1 → предел точности float в марше
// исчезает, а видимых уровней детализации — константа на любой глубине (точное самоподобие
// губки; потолок глубины ~24 уровня задаёт бюджет double на CPU).
//
// Геометрия в локальном кадре: свои уровни (текущая ячейка и мельче) — периодический
// IFS-фолдинг, как у корневой формулы (самоподобие: офсеты спуска кратны 2·3^k и mod-2
// фолдингом сокращаются); уровни ПРЕДКОВ (кресты, прорезающие окрестность, и внешний бокс) —
// K офсетов из буфера кадра, дальше их прячет туман. Скайбокс-путь прежний: куб вокруг камеры,
// skybox_vs, v_dir — направление луча (ротация ребейзом не меняется), глубина ровно 1.0.
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

// Кадр фрактала (пишет апдейтер FRACTAL_FRAME_BUFFER): [0] = (позиция камеры в локальном
// кадре, глубина d), [1] = (K, tFar, 1/дальность тумана, 0), [2+j] = (офсет предка j+1
// уровней вверх, 3^(j+1)). tFar/туман — в локальных юнитах, но ∝σ=s·3^d, т.е. НЕПРЕРЫВНЫ
// по масштабу s: смена якорного уровня не даёт шва по дальности; tFar = дальность насыщения
// тумана (дальше и хит, и промах — чистый sky, марш не нужен).
// Origin луча идёт отсюда (пост-пересчёт кадра), ротация — в v_dir ещё из VS.
StructuredBuffer<float4> Frame : register(t2, space2);

// Камера пасса — ТОЛЬКО для SV_Depth (этап 6 якорённых объектов): глубина хита считается
// тем же mul(proj, mul(view, p)), что у вершинника объектов, поэтому depth-тест сравнивает
// строго одинаковые значения (конвенция проекции не важна — матрицы общие). view — чистая
// ротация: аккумулятор позиции обнулён MengerTick'ом до снапшота. Буфер реально
// используется → DXC не стрипает, слоты storage-буферов остаются плотными.
struct CameraData { float4x4 view; float4x4 proj; };
StructuredBuffer<CameraData> Camera : register(t3, space2);

// Push-константы sp "Fractal" (fragment слот 0). Раскладка = FractalPushData (mygame/FractalBackground.h).
cbuffer FractalParams : register(b0, space3)
{
    float time;        // медленное вращение источника света
    uint  max_steps;   // потолок шагов рэймарча
    float _pad0;
    float _pad1;
};

struct PSInput
{
    float4 sv_pos : SV_Position;
    [[vk::location(0)]] float3 v_dir : TEXCOORD0;
};

struct PSOutput
{
    MAIN_PASS_TARGETS               // ambient = 0: у губки СВОЙ AO (по числу шагов марша), экранный ей не нужен
    // Честная глубина хита (DepthReplacing): растеризатор глубину этого дроу не считает
    // (вершины скайбокса на z=w), её выдаёт шейдер — в ОБЫЧНЫЙ depth-attachment пасса.
    // Стенки губки и якорённые объекты режут друг друга штатным depth-тестом; цена —
    // отключённый early-Z у этого дроу. Промах/небо — 1.0 (far), как раньше.
    float  depth    : SV_Depth;
};

static const float K_PX       = 8e-4;   // угловой размер пикселя (~1080p@45°): eps(t) = t·K_PX
static const int   MAX_LEVELS = 12;     // потолок СВОИХ уровней: мельче не видно на любом t

// GLSL-мод (floor-периодический): HLSL fmod знаковый и для фолдинга не годится.
float3 glmod(float3 x, float y) { return x - y * floor(x / y); }

float sdBox(float3 p, float3 b)
{
    float3 q = abs(p) - b;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
}

// Крест 27-разбиения ячейки [-1,1] в её юнитах (= уровень m=0 цикла ниже; делитель 3 как в IQ).
float MengerCross(float3 q)
{
    float3 a = glmod(q, 2.0) - 1.0;
    float3 r = abs(1.0 - 3.0 * abs(a));
    float da = max(r.x, r.y);
    float db = max(r.y, r.z);
    float dc = max(r.z, r.x);
    return (min(da, min(db, dc)) - 1.0) / 3.0;
}

// DE в локальном кадре: бокс K уровней вверх (K=0 → корень) + кресты предков (офсеты из
// кадра) + свои уровни периодическим фолдингом (levels — адаптив от дистанции).
float MengerDE(float3 p, int levels, uint K)
{
    // [loop]: DXC инлайнит MengerDE в 5 мест (марш + 4 тапа нормали) — без атрибута
    // анроллинг runtime-циклов раздувает SPIR-V до отказа пайплайн-компиляции драйвера.
    float4 fb = float4(0.0, 0.0, 0.0, 1.0);
    if (K > 0u) fb = Frame[1 + K];
    float d = sdBox(p / fb.w + fb.xyz, float3(1.0, 1.0, 1.0)) * fb.w;
    [loop] for (uint j = 0; j < K; ++j) {
        float4 f = Frame[2 + j];
        d = max(d, MengerCross(p / f.w + f.xyz) * f.w);
    }

    float s = 1.0;
    [loop] for (int m = 0; m < levels; ++m) {
        float3 a = glmod(p * s, 2.0) - 1.0;
        s *= 3.0;
        float3 r = abs(1.0 - 3.0 * abs(a));
        float da = max(r.x, r.y);
        float db = max(r.y, r.z);
        float dc = max(r.z, r.x);
        d = max(d, (min(da, min(db, dc)) - 1.0) / s);
    }
    return d;
}

// Сколько СВОИХ уровней раскрывать на дистанции t (в локальных юнитах): деталь мельче
// пиксельного футпринта не видна — не платим. 1/log2(3) ≈ 0.63093.
int LevelsFor(float t)
{
    float need = log2(1.0 / max(t * K_PX, 1e-12)) * 0.63093;
    return clamp((int)need + 1, 1, MAX_LEVELS);
}

// Палитра для лёгкой вариации альбедо по высоте локального кадра.
PSOutput main(PSInput input)
{
    PSOutput o;

    float3 ro = Frame[0].xyz;              // позиция камеры в локальном кадре (пост-ребейз)
    uint   K  = (uint)Frame[1].x;
    float3 rd = normalize(input.v_dir);    // ротация из view (VS) — ребейз её не трогает

    float3 sky = lerp(float3(0.05, 0.06, 0.12), float3(0.010, 0.012, 0.030),
                      saturate(rd.y * 2.0 + 0.2));
    float3 col = sky;
    float  depthOut = 1.0;   // промах/туманная даль = far, как у скайбокса

    float t      = 0.0;
    float tFar   = Frame[1].y;   // дальность насыщения тумана (CPU: FOG_DIST·σ) — дальше только sky
    float fogInv = Frame[1].z;   // 1/дальность тумана, непрерывна по масштабу s
    bool  culled = false;

    // На корневом уровне камера обычно СНАРУЖИ — слаб-тест бокса отрезает пустой подлёт и
    // ранний промах. Внутри (K>0) march стартует с нуля — стены рядом по построению кадра.
    if (K == 0u) {
        float3 rds = sign(rd) * max(abs(rd), 1e-9);
        float3 inv = 1.0 / rds;
        float3 lo  = (-1.0 - ro) * inv;
        float3 hi  = ( 1.0 - ro) * inv;
        float3 tn3 = min(lo, hi);
        float3 tf3 = max(lo, hi);
        float tEnter = max(max(tn3.x, tn3.y), tn3.z);
        float tExit  = min(min(tf3.x, tf3.y), tf3.z);
        if (tExit < max(tEnter, 0.0)) culled = true;
        else { t = max(tEnter, 0.0); tFar = min(tFar, tExit + 1e-3); }
    }

    if (!culled)
    {
        // Sphere tracing с over-relaxation (Keinert et al., Enhanced Sphere Tracing): шаг ω·d
        // вместо d — скольжение вдоль стен (худший случай: шаг ≈ зазору) дешевле в ~ω раз.
        // Если раздутый шаг проскочил поверхность между сферами (r+prev < шага) — откат на
        // весь шаг и дальше без релаксации. Параллельно копим КАНДИДАТА (минимальный угловой
        // промах r/t): исчерпание потолка шагов = луч прижат к поверхности → шейдим кандидата,
        // а не небо (раньше близкая геометрия при параллельном взгляде «исчезала»).
        bool  hit    = false;
        uint  steps  = 0;
        float omega  = 1.6;
        float prev_r = 0.0;
        float step_l = 0.0;
        float cand_t = t;
        float cand_e = 1e30;
        float capMiss = -1.0;   // >=0: луч упёрся в потолок шагов; значение — угловой промах кандидата
        [loop] for (; steps < max_steps; ++steps)
        {
            float r = MengerDE(ro + rd * t, LevelsFor(t), K);
            if (omega > 1.0 && (r + prev_r) < step_l) {
                t -= step_l;      // откат в безопасную точку, релаксацию этому лучу выключаем
                omega  = 1.0;
                prev_r = 0.0;
                step_l = 0.0;
                continue;
            }
            float eps = max(t * K_PX, 1e-7);
            if (r < eps) { hit = true; break; }
            float miss = r / max(t, 1e-6);
            if (miss < cand_e) { cand_e = miss; cand_t = t; }
            prev_r = r;
            step_l = omega * r;
            t += step_l;
            if (t > tFar) break;
        }
        if (!hit && steps >= max_steps) { hit = true; t = cand_t; capMiss = cand_e; }   // потолок = хит-кандидат

        if (hit)
        {
            // Нормаль — тетраэдральный градиент DE на масштабе eps попадания.
            float h = max(t * K_PX, 2e-6);
            int   lv = LevelsFor(t);
            float2 e = float2(1.0, -1.0) * 0.57735;
            float3 n = normalize(
                e.xyy * MengerDE(ro + rd * t + e.xyy * h, lv, K) +
                e.yyx * MengerDE(ro + rd * t + e.yyx * h, lv, K) +
                e.yxy * MengerDE(ro + rd * t + e.yxy * h, lv, K) +
                e.xxx * MengerDE(ro + rd * t + e.xxx * h, lv, K));

            // Ключевой свет медленно кружит (time из push) + небо сверху; AO — из числа
            // шагов марша (щели и углы «доходят» дольше → темнее).
            float3 L    = normalize(float3(cos(time * 0.1), 0.75, sin(time * 0.1)));
            float  diff = max(dot(n, L), 0.0);
            float  amb  = 0.22 + 0.18 * n.y;
            // Пол AO: хиты-кандидаты по потолку шагов (steps==max) — тёмные, но не чёрные.
            float  ao   = max(pow(saturate(1.0 - (float)steps / (float)max_steps), 1.5), 0.06);

            // Альбедо ТОЛЬКО от нормали (масштабно-инвариантно): привязка к локальной
            // позиции прыгала бы гаммой при каждом ребейзе (координаты кадра меняются,
            // мировая точка — нет). Верхние грани — тёплые, боковые/нижние — холодные.
            float3 albedo = lerp(float3(0.40, 0.44, 0.52),
                                 float3(0.90, 0.72, 0.48),
                                 saturate(n.y * 0.5 + 0.5));

            col = albedo * (amb + 0.9 * diff) * ao;
            // Туман: дальность из кадра (∝σ — непрерывна по масштабу). Насыщается ровно на
            // tFar, поэтому обрез марша по tFar пиксельно-точен; обрез K-контекста предков
            // (грань 8-го уровня на 3^8 ≈ 6561) туман по-прежнему прячет.
            col = lerp(col, sky, saturate(t * fogInv));
            // Потолочные хиты: чем больше угловой промах кандидата (луч летит в просвет
            // поры/шахты, а не прижат к поверхности), тем сильнее уводим в небо — глубина
            // пор читается атмосферой, а не чёрными квадратами; скользящий горизонт стены
            // (промах < пикселя) остаётся тёмной поверхностью.
            if (capMiss >= 0.0)
                col = lerp(col, sky, saturate(capMiss / (K_PX * 8.0)) * 0.9);

            // Глубина хита — ТА ЖЕ проекция, что у вершин якорённых объектов (камера растра
            // в нуле кадра, юниты якоря общие): сравнение в depth-тесте один-в-один.
            float4 clipH = mul(Camera[0].proj, mul(Camera[0].view, float4(rd * t, 1.0)));
            depthOut = saturate(clipH.z / max(clipH.w, 1e-9));
        }
    }

    o.color    = float4(col, 1.0);
    o.emission = float4(0.0, 0.0, 0.0, 0.0);
    o.ambient  = float4(0.0, 0.0, 0.0, 0.0);
    o.depth    = depthOut;

    // ЯКОРЬ t0/t1: ветка никогда не выполняется (sv_pos.x >= 0 во вьюпорте), но компилятор
    // доказать этого не может → глобалки пасса не стрипаются, слоты остаются плотными.
    if (input.sv_pos.x < -1.0) {
        o.color.r += u_shadowDepthArray.SampleCmpLevelZero(u_shadowSampler, float3(0.0, 0.0, 0.0), 0.0);
        o.color.g += u_envCube.Sample(u_envSampler, float3(0.0, 1.0, 0.0)).g;
    }

    return o;
}
