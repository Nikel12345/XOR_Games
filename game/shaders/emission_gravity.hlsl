#ifndef EMISSION_GRAVITY_HLSL
#define EMISSION_GRAVITY_HLSL

// Игровой бестекстурный surface: тот же LitColor, но эмиссия фрагмента растёт по мере
// приближения к ближайшему центру гравитации. Вдали от всех центров (дальше
// GRAVITY_EMISSION_RADIUS) эмиссии нет совсем. Ближе GRAVITY_WHITE_RADIUS цвет эмиссии
// (emissive материала) вдобавок уходит к белому — в центре он белый целиком.
//
// Фрагментник общий для всех трёх уровней LOD (модель / квад / точка): их вершинники выдают
// одинаковый PSInput, и свечение не скачет при смене уровня.

#include "main_pass/untextured/material_api.hlsl"

// Значения приходят из "defines" записи шейдера в shaders.json сцены — там их единственный источник:
//   GRAVITY_EMISSION_RADIUS  дистанция (юниты мира), на которой эмиссия гаснет до нуля;
//   GRAVITY_EMISSION_POWER   степень спада яркости (2 — квадратичный);
//   GRAVITY_WHITE_RADIUS     дистанция, с которой цвет эмиссии начинает уходить к белому;
//   GRAVITY_WHITE_POWER      степень ухода к белому.
#if !defined(GRAVITY_EMISSION_RADIUS) || !defined(GRAVITY_EMISSION_POWER) || \
    !defined(GRAVITY_WHITE_RADIUS)    || !defined(GRAVITY_WHITE_POWER)
#error "emission_gravity.hlsl: GRAVITY_* defines must come from the shader manifest"
#endif

// Раскладка совпадает с OpaqueMaterialParams (C++), как у движкового LitColor.
cbuffer MaterialBlock : MATERIAL_BLOCK_REGISTER {
    float4 baseColor;
    float3 emissive;
    float  emissiveStrength;
    float  metallic;
    float  roughness;
};

StructuredBuffer<float4> GravityCenters : register(t5, space2);

// Встаёт за счётчиком светов (b0) и параметрами материала (b1) — порядок маркеров = порядок слотов.
//@push gravity_centers
cbuffer GravityCentersBlock : register(b2, space3) {
    uint u_gravityCount;
};

SurfaceData getSurface(PSInput input, bool isFrontFace)
{
    float nearest = GRAVITY_EMISSION_RADIUS;
    for (uint i = 0; i < u_gravityCount; ++i)
        nearest = min(nearest, distance(input.v_worldPos, GravityCenters[i].xyz));
    const float closeness = 1.0 - nearest / GRAVITY_EMISSION_RADIUS;   // [0..1], 1 = в центре
    const float whiteness = saturate(1.0 - nearest / GRAVITY_WHITE_RADIUS);
    const float3 tint     = lerp(emissive, (float3)1.0, pow(whiteness, GRAVITY_WHITE_POWER));

    SurfaceData s;
    s.baseColor = baseColor.rgb;
    float3 n    = normalize(input.v_worldNormal);
    s.normal    = isFrontFace ? n : -n;
    s.alpha     = baseColor.a * input.v_alpha;
    s.emission  = tint * emissiveStrength * pow(closeness, GRAVITY_EMISSION_POWER);
    s.metallic  = metallic;
    s.roughness = roughness;
    s.ao        = 1.0;
    return s;
}

#include "main_pass/main_pass.frag.hlsl"

#endif
