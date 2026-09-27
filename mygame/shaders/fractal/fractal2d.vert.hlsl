// 2D-фрактал: фуллскрин-квад БЕЗ камеры — картинка НЕ зависит от поворота камеры,
// пан/зум фрагментник берёт только из её ПОЗИЦИИ. Модель "skybox_cube" во фрактальном
// наборе (DefaultResources) — NDC-квад [-1,1]²; сюда он приходит уже готовыми NDC-координатами,
// z=w=1 → глубина ровно 1.0: фон проходит LESS_OR_EQUAL только по клиру (как скайбокс).

struct VSInput
{
    float3 a_pos : POSITION;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 v_ndc    : TEXCOORD0;   // NDC пикселя (w=1 у всех вершин → интерполяция линейна)
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.position = float4(input.a_pos.xy, 1.0, 1.0);
    output.v_ndc    = input.a_pos.xy;
    return output;
}
