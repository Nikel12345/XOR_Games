#ifndef BUILDING_SURFACE_HLSL
#define BUILDING_SURFACE_HLSL

// Игровой surface: тот же PBR, что у движкового Lit, но с ПОВТОРОМ текстуры по UV.
//
// Зачем. Атлас повторять нельзя: sampleAtlas (main_pass/material.hlsl) пересчитывает координату
// в прямоугольник тайла (uv*scale + offset), и uv > 1 уехал бы в СОСЕДНЮЮ текстуру атласа —
// поэтому режима REPEAT у сэмплера для атласного материала не существует в принципе. Из-за этого
// повтор приходилось делать геометрией: чтобы текстура стены легла 30 раз по высоте, генератор
// клал 30 квадов с uv 0..1. На городе это 76% всех вершин.
//
// Здесь повтор берётся вручную — frac(uv) ДО пересчёта в атлас. Тогда один квад с uv 0..30 даёт
// ровно ту же картинку, что 30 квадов с uv 0..1, и вершины на это не тратятся.
//
// Геометрия обязана приходить с uv в ЕДИНИЦАХ ТАЙЛА (uv = 8 значит восемь повторов) — материалу
// с этой программой нельзя давать меш, размеченный под Lit, и наоборот.

#include "main_pass/material_api.hlsl"
#include "shaders/tiled_atlas.hlsli"

// Раскладка совпадает с OpaqueMaterialParams (params_type "Opaque") — материалу не нужен свой
// тип параметров. heightScale/pomBias здесь МЁРТВЫЕ: POM марширует по mesh-UV, а он тут не в
// [0..1], и марч ушёл бы через швы тайлов. Поля оставлены, чтобы блоб не разъехался с C++.
cbuffer MaterialBlock : MATERIAL_BLOCK_REGISTER {
    float4 baseColorFactor;
    float3 emissive;
    float  emissiveStrength;
    float  metallic;
    float  roughness;
    float  heightScale;
    float  pomBias;
};

SurfaceData getSurface(PSInput input, bool isFrontFace)
{
    // Производные берём ОДИН раз и до всего остального: деривативы легальны только в uniform
    // control flow, а дальше их получает каждая выборка.
    float2 uv     = input.v_uv;
    float2 ddx_uv = ddx(uv);
    float2 ddy_uv = ddy(uv);

    SurfaceData s;
    float4 alb  = sampleAtlasTiled(u_albedo, u_albedoSampler, textures[TexIndex(0)].data,
                                   uv, ddx_uv, ddy_uv, 0.0);
    s.baseColor = alb.rgb * baseColorFactor.rgb;
    s.alpha     = alb.a * input.v_alpha;

    float3 nw = tiledNormalWorld(u_normal, u_normalSampler, textures[TexIndex(1)].data,
                                 uv, ddx_uv, ddy_uv,
                                 input.v_worldTangent, input.v_worldBitangent, input.v_worldNormal);
    s.normal = isFrontFace ? nw : -nw;

    // ORM: R=AO, G=Roughness, B=Metallic; текстура — множитель фактора (как в Lit).
    float3 orm  = sampleAtlasTiled(u_orm, u_ormSampler, textures[TexIndex(2)].data,
                                   uv, ddx_uv, ddy_uv, 0.0).rgb;
    s.ao        = orm.r;
    s.roughness = roughness * orm.g;
    s.metallic  = metallic  * orm.b;

    s.emission = sampleAtlasTiled(u_emissive, u_emissiveSampler, textures[TexIndex(3)].data,
                                  uv, ddx_uv, ddy_uv, 0.0).rgb * emissive * emissiveStrength;
    return s;
}

#include "main_pass/main_pass.frag.hlsl"

#endif
