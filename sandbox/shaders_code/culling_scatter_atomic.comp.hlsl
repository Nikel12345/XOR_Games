// Отсев одного прохода: поток на запись PIB. Запись выбирает уровень (по камере игрока, один раз)
// и для каждого блока прохода (камеры) при видимости кладёт свою строку в кусок out_pib своего
// группового уровня. Команды после этого переписывает culling_fixup.
//
// Раскладка out_pib и счётчиков в регионе прохода (всё в элементах):
//   out_pib:  out_base + b * out_cap + group.out_offset + L * group.size + слот
//   счётчик:  cnt_base + b * gl_count + group.gl_base + L

struct GroupEntry {
    uint  size;         // записей в группе
    uint  out_offset;   // кусок группы внутри блока прохода
    uint  gl_base;      // первый групповой уровень группы в проходе
    uint  lod_count;
    float switches[3];  // экранный радиус (px), ниже которого уровень L уступает L+1
    uint  pad;
};
struct CameraData { float4x4 view; float4x4 proj; };

StructuredBuffer<int>        PIB          : register(t0, space0);   // запись -> строка (-1 = нет строки)
StructuredBuffer<uint>       RecordGroup  : register(t1, space0);   // запись -> группа
StructuredBuffer<GroupEntry> Groups       : register(t2, space0);
StructuredBuffer<float4>     BoundSpheres : register(t3, space0);   // по строкам; w<0 = геометрии нет
StructuredBuffer<float4x4>   Transforms   : register(t4, space0);
StructuredBuffer<CameraData> Cameras      : register(t5, space0);   // камеры блоков прохода
StructuredBuffer<CameraData> LodCamera    : register(t6, space0);   // [0] — камера игрока

RWStructuredBuffer<int>  OutPib   : register(u0, space1);
RWStructuredBuffer<uint> Counters : register(u1, space1);

cbuffer ScatterParams : register(b0, space2) {
    uint  range_start;
    uint  range_count;
    uint  num_blocks;
    uint  out_base;
    uint  out_cap;
    uint  cnt_base;
    uint  gl_count;
    uint  target_height;         // высота таргета камеры игрока: пиксели уровня
    float min_screen_radius_px;  // 0 = отсев мелочи выключен
};

// Радиус сферы в пикселях. w клипа = dot(vp[3], p); proj[1][1] — вертикальный фокус.
// Вызывать для сфер перед камерой: иначе w <= 0.
float ScreenRadiusPx(float4x4 vp, float4x4 proj, float3 center, float radius)
{
    float w = dot(vp[3], float4(center, 1.0));
    return radius * proj[1][1] * (0.5 * float(target_height)) / max(w, 1e-4);
}

bool SphereVisible(float4x4 vp, float3 center, float radius)
{
    float4 planes[6] = {
        vp[3] + vp[0], vp[3] - vp[0],
        vp[3] + vp[1], vp[3] - vp[1],
        vp[3] + vp[2], vp[3] - vp[2],
    };
    [unroll]
    for (int p = 0; p < 6; ++p) {
        if (dot(planes[p].xyz, center) + planes[p].w < -radius * length(planes[p].xyz))
            return false;
    }
    return true;
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= range_count) return;
    uint i = range_start + tid.x;

    int row = PIB[i];
    GroupEntry g = Groups[RecordGroup[i]];

    // [branch] обязателен: flatten прочитал бы BoundSpheres[-1].
    float4 sphere = float4(0.0, 0.0, 0.0, -1.0);
    [branch] if (row >= 0) sphere = BoundSpheres[row];
    bool has_geom = (sphere.w >= 0.0);
    float3 center = float3(0, 0, 0);
    float  radius = 0.0;
    if (has_geom) {
        float4x4 m = Transforms[row];
        center = mul(m, float4(sphere.xyz, 1.0)).xyz;
        float3 sc = float3(
            length(float3(m[0][0], m[1][0], m[2][0])),
            length(float3(m[0][1], m[1][1], m[2][1])),
            length(float3(m[0][2], m[1][2], m[2][2])));
        radius = sphere.w * max(sc.x, max(sc.y, sc.z));
    }

    // Уровень один на все блоки: тень отбрасывает тот же уровень, что виден игроку.
    uint L = 0;
    if (has_geom && g.lod_count > 1) {
        float4x4 lod_vp = mul(LodCamera[0].proj, LodCamera[0].view);
        float px = ScreenRadiusPx(lod_vp, LodCamera[0].proj, center, radius);
        while (L + 1 < g.lod_count && px < g.switches[L]) ++L;
    }

    uint chunk = g.out_offset + L * g.size;
    uint level = g.gl_base + L;
    for (uint b = 0; b < num_blocks; ++b) {
        if (has_geom) {
            float4x4 vp = mul(Cameras[b].proj, Cameras[b].view);
            if (!SphereVisible(vp, center, radius)) continue;
            if (min_screen_radius_px > 0.0 && ScreenRadiusPx(vp, Cameras[b].proj, center, radius) < min_screen_radius_px)
                continue;
        }
        uint slot;
        InterlockedAdd(Counters[cnt_base + b * gl_count + level], 1u, slot);
        OutPib[out_base + b * out_cap + chunk + slot] = row;
    }
}
