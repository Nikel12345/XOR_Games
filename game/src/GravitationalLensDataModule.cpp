#include "PCH.h"
#include "GravitationalLensDataModule.h"
#include "ObjectManager.h"
#include "BaseComponents.h"
#include "PositionStructure.h"
#include "BufferManager.h"
#include "BufferUpdateStruct.h"
#include "GameComponents.h"

void GravitationalLensDataModule::Stamp(ObjectManager* object_manager, uint8_t slot)
{
    std::vector<GravitationalLensGpu>& slot_lenses = lenses[slot];
    slot_lenses.clear();
    SceneData* scene = object_manager->GetActiveScene();
    if (!scene) return;

    object_manager->ForEach<Positions, GravitationalLensComponent>(scene,
        [&slot_lenses](SoAElement<Positions> position, GravitationalLensComponent& lens)
    {
        const Positions& positions = position.container();
        const size_t row = position.i();
        GravitationalLensGpu& gpu_lens = slot_lenses.emplace_back();
        gpu_lens.world_center[0] = positions.w[row];
        gpu_lens.world_center[1] = positions.d[row];
        gpu_lens.world_center[2] = positions.h[row];
        gpu_lens.schwarzschild_radius = lens.schwarzschild_radius;
        gpu_lens.inner_radius = lens.inner_radius;
    });
}

uint32_t GravitationalLensDataModule::Size(uint8_t slot) const
{
    return safe_u32(lenses[slot].size() * sizeof(GravitationalLensGpu));
}

void GravitationalLensDataModule::Store(BufferManager* buffer_manager, UploadTask* task, uint8_t slot) const
{
    if (lenses[slot].empty()) return;
    buffer_manager->UploadToTransferBuffer(task, Size(slot), lenses[slot].data());
}

uint32_t GravitationalLensDataModule::AskLensCount(uint8_t slot) const
{
    return safe_u32(lenses[slot].size());
}
