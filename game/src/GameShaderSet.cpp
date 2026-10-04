#include "PCH.h"
#include "GameShaderSet.h"
#include "EngineContext.h"
#include "ShaderManager.h"
#include "GravityCenterDataModule.h"

void GameShaderSet::RegisterShaderFuncs(EngineContext* ctx, GravityCenterDataModule* gravity_center_data)
{
    // Типовой пуш: его получает любая программа, чей шейдер несёт маркер //@push gravity_centers
    // (shaders/emission_gravity.hlsl) — все уровни LOD светящегося материала разом.
    ctx->GetShaderManager()->RegisterPushKind("gravity_centers", PushStage::Fragment,
        [gravity_center_data](const PushConstantBinder& binder, const PushInput&) {
            binder.Push(GravityCenterCountPushData{ gravity_center_data->AskCenterCount(binder.frame), {} });
        });
}
