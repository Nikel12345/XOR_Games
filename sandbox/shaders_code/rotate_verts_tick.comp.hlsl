// Вращалка многопоточного зонда: как rotate_verts.comp.hlsl, но дельта приходит НЕ пушем, а
// из ПЕР-СЛОТОВОГО буфера, который залил sim-поток на копировальной очереди.
//
// Это и есть смысл варианта: в одно­поточном зонде тик просто лежал в глобальной переменной и
// доезжал до шейдера пушем, записанным тем же потоком. Здесь он проходит настоящий путь
// движка — sim пишет свой слот, копировальная очередь заливает, render читает, — то есть
// проверяются и пер-слотовая раскладка Dynamic-буфера, и межпоточная передача, а не только
// маршрутизация команд.
//
// Буфер тика ОДНОЭЛЕМЕНТНЫЙ: BufferManager сам подставляет нужную копию слота при бинде
// (Dynamic-буферы пер-слотовые), поэтому индексироваться по слоту в шейдере не нужно и НЕЛЬЗЯ —
// шейдер видит ровно свою.

StructuredBuffer<float>   Tick  : register(t0, space0);   // [0] = дельта угла этого слота
RWStructuredBuffer<float> Verts : register(u0, space1);   // x,y,z подряд на вершину

cbuffer RotateParams : register(b0, space2) {
    uint vertex_count;
};

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= vertex_count) return;

    uint base = i * 3u;
    float x = Verts[base + 0u];
    float y = Verts[base + 1u];

    float s, c;
    sincos(Tick[0], s, c);

    Verts[base + 0u] = x * c - y * s;
    Verts[base + 1u] = x * s + y * c;
}
