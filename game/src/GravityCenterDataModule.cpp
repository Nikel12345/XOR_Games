#include "PCH.h"
#include "GravityCenterDataModule.h"
#include "ObjectManager.h"
#include "BaseComponents.h"
#include "PositionStructure.h"
#include "BufferManager.h"
#include "BufferUpdateStruct.h"
#include "GameComponents.h"

void GravityCenterDataModule::Stamp(ObjectManager* object_manager, uint8_t slot)
{
    std::vector<GravityCenterGpu>& slot_centers = centers[slot];
    slot_centers.clear();
    SceneData* scene = object_manager->GetActiveScene();
    if (!scene) return;

    object_manager->ForEach<Positions, GravityComponent>(scene,
        [&slot_centers](SoAElement<Positions> position, GravityComponent&)
    {
        const Positions& positions = position.container();
        const size_t row = position.i();
        GravityCenterGpu& gpu_center = slot_centers.emplace_back();
        gpu_center.world_center[0] = positions.w[row];
        gpu_center.world_center[1] = positions.d[row];
        gpu_center.world_center[2] = positions.h[row];
    });
}

uint32_t GravityCenterDataModule::Size(uint8_t slot) const
{
    return safe_u32(centers[slot].size() * sizeof(GravityCenterGpu));
}

void GravityCenterDataModule::Store(BufferManager* buffer_manager, UploadTask* task, uint8_t slot) const
{
    if (centers[slot].empty()) return;
    buffer_manager->UploadToTransferBuffer(task, Size(slot), centers[slot].data());
}

uint32_t GravityCenterDataModule::AskCenterCount(uint8_t slot) const
{
    return safe_u32(centers[slot].size());
}
