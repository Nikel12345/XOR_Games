#ifndef PODIUM_SURFACE_HLSL
#define PODIUM_SURFACE_HLSL

// Прозрачный игровой surface с ПОВТОРОМ текстуры — стекло цоколя.
// Тот же приём, что у building_surface.hlsl (frac(uv) до пересчёта в атлас), но на прозрачном
// проходе: у него свой пролог, свои регистры (теней нет, сэмплеры с t0) и своя SurfaceData
// без PBR-полей. Геометрия обязана приходить с uv в ЕДИНИЦАХ ТАЙЛА.

#include "transparent_pass/material_api.hlsl"
#include "shaders/tiled_atlas.hlsli"

// Дублирует TransparentMaterialParams (C++) — params_type материала остаётся "Transparent".
cbuffer MaterialBlock : MATERIAL_BLOCK_REGISTER {
    float u_alpha;
};

SurfaceData getSurface(PSInput input, bool isFrontFace)
{
    // Производные — один раз и до всего: деривативы легальны только в uniform control flow.
    float2 uv     = input.v_uv;
    float2 ddx_uv = ddx(uv);
    float2 ddy_uv = ddy(uv);

    SurfaceData s;
    float4 alb  = sampleAtlasTiled(u_albedo, u_albedoSampler, textures[TexIndex(0)].data,
                                   uv, ddx_uv, ddy_uv, 0.0);
    s.baseColor = alb.rgb;
    s.alpha     = alb.a * u_alpha * input.v_alpha;   // текстура × per-material × per-instance

    float3 n = tiledNormalWorld(u_normal, u_normalSampler, textures[TexIndex(1)].data,
                                uv, ddx_uv, ddy_uv,
                                input.v_worldTangent, input.v_worldBitangent, input.v_worldNormal);
    s.normal = isFrontFace ? n : -n;
    return s;
}

#include "transparent_pass/transparent.frag.hlsl"

#endif
