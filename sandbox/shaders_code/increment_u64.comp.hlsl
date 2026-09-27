// Зонд автобарьеров (см. sandbox/src/CopyComputeBarrierProbe.cpp): +1 к КАЖДОМУ элементу.
// Элемент = 64-битное число как uint2 (little-endian: .x = low, .y = high) — тот же вес,
// что double (8 байт), но без зависимости от shaderFloat64 (SDL_gpu его девайсу не включает).
// Заливка кладёт 1 (uint2(1,0)), после инкремента ждём 2 (uint2(2,0)).
//
// Диспатч 2D: линейный индекс = y * row_elems + x. Одномерный на 300 МБ упёрся бы в
// гарантированный Vulkan-минимум лимита групп (65535 на измерение).

RWStructuredBuffer<uint2> Data : register(u0, space1);

cbuffer IncParams : register(b0, space2) {
    uint total_elements;
    uint row_elems;       // элементов в «строке» диспатча; x внутри строки не выходит за неё
};

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.y * row_elems + tid.x;
    if (i >= total_elements) return;
    uint2 v = Data[i];
    uint lo = v.x + 1u;
    uint hi = v.y + (lo < v.x ? 1u : 0u);   // перенос: элемент — честный uint64
    Data[i] = uint2(lo, hi);
}
