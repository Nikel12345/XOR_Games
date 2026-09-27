#include "PCH.h"
#include "GameShaderSet.h"
#include "EngineContext.h"
#include "ShaderManager.h"

namespace
{
    // sim пишет раз в тик, render читает на каждый draw программ со свечением — копия в 144 байта
    // под мьютексом дешевле любой схемы слотов. Кадр рассинхрона с позициями кубов не виден:
    // центры сим не двигает.
    std::mutex g_gravity_mutex;
    GameShaderSet::GravityCentersPushData g_gravity{};
}

void GameShaderSet::PublishGravityCenters(const float (*xyz)[3], size_t count)
{
    GravityCentersPushData d{};
    d.count = safe_u32(std::min(count, MAX_GRAVITY_CENTERS));
    for (uint32_t i = 0; i < d.count; ++i) {
        d.centers[i][0] = xyz[i][0];
        d.centers[i][1] = xyz[i][1];
        d.centers[i][2] = xyz[i][2];
    }
    std::lock_guard lock(g_gravity_mutex);
    g_gravity = d;
}

void GameShaderSet::RegisterShaderFuncs(EngineContext* ctx)
{
    // Типовой пуш: его получает любая программа, чей шейдер несёт маркер //@push gravity_centers
    // (shaders/emission_gravity.hlsl) — все уровни LOD светящегося материала разом.
    ctx->GetShaderManager()->RegisterPushKind("gravity_centers", PushStage::Fragment,
        [](const PushConstantBinder& b, const PushInput&) {
            GravityCentersPushData d;
            {
                std::lock_guard lock(g_gravity_mutex);
                d = g_gravity;
            }
            b.Push(d);
        });
}
