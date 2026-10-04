#pragma once

#include <cstdint>

class EngineContext;
class GravityCenterDataModule;

// Место под СВОИ код-байндинги шейдеров этой игры. Движковый набор compute-программ
// (каллинг/bloom/blur) живёт в движке — DefaultShaderSet.h.
namespace GameShaderSet
{
    struct alignas(16) GravityCenterCountPushData {
        uint32_t count;
        uint32_t padding[3];
    };

    // Push/dispatch — ИНСТРУКЦИИ ПО ИМЕНИ в реестре ShaderManager: зовётся ОДИН РАЗ на
    // инициализации, ДО первой LoadScene, а вешает функции на sp сам движок (и на создании
    // программы, и общим проходом в конце каждой загрузки).
    void RegisterShaderFuncs(EngineContext* ctx, GravityCenterDataModule* gravity_center_data);
}
