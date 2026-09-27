// Фрагментник зонда очередей: цвет как есть, без освещения, текстур и материалов.
// Ни одного бинда — sp объявляется без буферов и без слотов текстур, поэтому проход
// не тянет за собой ни материалов, ни глобальных сэмплеров.

struct PSInput
{
    float4 sv_pos                       : SV_Position;
    [[vk::location(0)]] float3 v_color  : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target0
{
    return float4(input.v_color, 1.0);
}
