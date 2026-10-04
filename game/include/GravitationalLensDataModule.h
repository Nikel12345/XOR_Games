#pragma once
#include <cstdint>
#include <vector>
#include "config.h"

class ObjectManager;
class BufferManager;
struct UploadTask;

struct GravitationalLensGpu {
    float    world_center[3];
    float    schwarzschild_radius;
    float    inner_radius;
    uint32_t padding[3];
};
static_assert(sizeof(GravitationalLensGpu) == 32);

class GravitationalLensDataModule {
public:
    void     Stamp(ObjectManager* object_manager, uint8_t slot);
    uint32_t Size(uint8_t slot) const;
    void     Store(BufferManager* buffer_manager, UploadTask* task, uint8_t slot) const;
    uint32_t AskLensCount(uint8_t slot) const;

private:
    std::vector<GravitationalLensGpu> lenses[BUFFERING_LEVEL];
};
