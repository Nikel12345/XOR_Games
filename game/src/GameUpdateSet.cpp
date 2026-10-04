#include "PCH.h"
#include "GameUpdateSet.h"
#include "EngineContext.h"
#include "BufferManager.h"
#include "BufferUpdateStruct.h"
#include "GravityCenterDataModule.h"
#include "GravitationalLensDataModule.h"

void GameUpdateSet::SetGravityCentersUpdater(EngineContext* ctx, GravityCenterDataModule* gravity_center_data)
{
    BufferManager* buffer_manager = ctx->GetBufferManager();
    ObjectManager* object_manager = ctx->GetObjectManager();

    buffer_manager->CreateBufferData(GRAVITY_CENTERS_BUFFER, sizeof(GravityCenterGpu), BufferDataType::Dynamic,
        ResizeBehaviour::RESIZE_ONLY, ResourceTag::CodeOwned | ResourceTag::System);
    buffer_manager->CreateUpdateInstruction(GRAVITY_CENTERS_BUFFER,
        [gravity_center_data](SDL_GPUCopyPass*, BufferManager* buffers, UploadTask& task) {
            gravity_center_data->Store(buffers, &task, buffers->logic_index.load()); },
        [gravity_center_data, object_manager, buffer_manager]() -> uint32_t {
            const uint8_t slot = buffer_manager->logic_index.load();
            gravity_center_data->Stamp(object_manager, slot);
            return gravity_center_data->Size(slot); });
}

void GameUpdateSet::SetGravitationalLensUpdater(EngineContext* ctx, GravitationalLensDataModule* gravitational_lens_data)
{
    BufferManager* buffer_manager = ctx->GetBufferManager();
    ObjectManager* object_manager = ctx->GetObjectManager();

    buffer_manager->CreateBufferData(GRAVITATIONAL_LENS_BUFFER, sizeof(GravitationalLensGpu), BufferDataType::Dynamic,
        ResizeBehaviour::RESIZE_ONLY, ResourceTag::CodeOwned | ResourceTag::System);
    buffer_manager->CreateUpdateInstruction(GRAVITATIONAL_LENS_BUFFER,
        [gravitational_lens_data](SDL_GPUCopyPass*, BufferManager* buffers, UploadTask& task) {
            gravitational_lens_data->Store(buffers, &task, buffers->logic_index.load()); },
        [gravitational_lens_data, object_manager, buffer_manager]() -> uint32_t {
            const uint8_t slot = buffer_manager->logic_index.load();
            gravitational_lens_data->Stamp(object_manager, slot);
            return gravitational_lens_data->Size(slot); });
}
