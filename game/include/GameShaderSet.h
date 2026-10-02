#pragma once

#include <cstdint>
#include <cstddef>

class EngineContext;

// Место под СВОИ код-байндинги шейдеров этой игры. Движковый набор compute-программ
// (каллинг/bloom/blur) живёт в движке — DefaultShaderSet.h.
namespace GameShaderSet
{
    // Совпадает с MAX_GRAVITY_CENTERS в shaders/emission_gravity.hlsl.
    inline constexpr size_t MAX_GRAVITY_CENTERS = 8;

    // Раскладка совпадает с GravityCentersBlock в shaders/emission_gravity.hlsl: массив в cbuffer
    // идёт с шагом 16 байт, поэтому центр — float4, w не используется.
    struct GravityCentersPushData {
        float    centers[MAX_GRAVITY_CENTERS][4];
        uint32_t count;
        uint32_t pad[3];
    };

    // Push/dispatch — ИНСТРУКЦИИ ПО ИМЕНИ в реестре ShaderManager: зовётся ОДИН РАЗ на
    // инициализации, ДО первой LoadScene, а вешает функции на sp сам движок (и на создании
    // программы, и общим проходом в конце каждой загрузки).
    void RegisterShaderFuncs(EngineContext* ctx);

    // Пишет sim-поток (GravitySystem::SimulateGravity), читает пуш gravity_centers на render-потоке.
    // Лишние центры сверх MAX_GRAVITY_CENTERS отбрасываются.
    void PublishGravityCenters(const float (*xyz)[3], size_t count);
}
