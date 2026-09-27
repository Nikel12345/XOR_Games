#ifndef TILED_ATLAS_HLSLI
#define TILED_ATLAS_HLSLI

// Повтор атласной текстуры по UV — общая часть игровых surface (main и прозрачный прологи
// объявляют РАЗНЫЕ регистры, но сама выборка одинакова, и разъехаться ей нельзя).
//
// Зачем вручную. sampleAtlas (main_pass/material.hlsl) пересчитывает координату в прямоугольник
// тайла: uv*scale + offset. uv > 1 уехал бы в СОСЕДНЮЮ текстуру атласа, поэтому режима REPEAT
// у сэмплера для атласного материала не существует в принципе, и повтор приходилось делать
// геометрией — по квадру на каждый шаг текстуры. Здесь его даёт frac, и геометрия кладёт
// один квад с uv 0..N.
//
// Включать ПОСЛЕ пролога пасса: отсюда нужны unpackUnorm2x16 (math.hlsl) и ничего больше.

// Координата внутри тайла: дробная часть, поджатая на полтексела внутрь.
// Инсет обязателен: frac даёт значения вплотную к 0 и 1, а билинейная фильтрация подмешивает
// соседний тексель — за краем тайла им оказывается ЧУЖАЯ текстура атласа (полей в атласе нет).
float2 tileUV(float2 uv, float2 scale, float2 texel)
{
    float2 inset = 0.5 * texel / scale;      // полтексела, выраженные в единицах тайла
    return lerp(inset, 1.0 - inset, frac(uv));
}

// Выборка с повтором. Градиенты — от НЕРАЗВЁРНУТОГО uv: на шве frac прыгает с 0.99 на 0, и
// автоматический LOD увидел бы там скачок в целую текстуру, уводя полосу пикселей в последний
// мип (размытая линия по каждому шву). SampleGrad снимает выбор мипа с рваной координаты.
// mip_bias эмулируется масштабом градиентов: +1 LOD = вдвое больший футпринт.
float4 sampleAtlasTiled(Texture2DArray atlas, SamplerState samp, uint4 td,
                        float2 uv, float2 ddx_uv, float2 ddy_uv, float mip_bias)
{
    float2 offset = unpackUnorm2x16(td.x);
    float2 scale  = unpackUnorm2x16(td.y);
    uint w, h, layers;
    atlas.GetDimensions(w, h, layers);

    float2 t = tileUV(uv, scale, 1.0 / float2(w, h));
    float  g = exp2(mip_bias);
    return atlas.SampleGrad(samp, float3(t * scale + offset, float(td.z)),
                            ddx_uv * scale * g, ddy_uv * scale * g);
}

// Зеркала констант computeNormal (main_pass/material.hlsl): своя выборка нормали нужна ради
// SampleGrad, но рельеф обязан выглядеть так же, как у движковых программ. Правка там = правка здесь.
static const float TILED_NORMAL_STRENGTH = 0.7;
static const float TILED_NORMAL_MIP_BIAS = 1.0;

float3 tiledNormalWorld(Texture2DArray atlas, SamplerState samp, uint4 td,
                        float2 uv, float2 ddx_uv, float2 ddy_uv,
                        float3 T, float3 B, float3 N)
{
    float3 n = sampleAtlasTiled(atlas, samp, td, uv, ddx_uv, ddy_uv, TILED_NORMAL_MIP_BIAS).rgb
             * 2.0 - 1.0;
    n.xy *= TILED_NORMAL_STRENGTH;
    return normalize(mul(n, float3x3(T, B, normalize(N))));
}

#endif
