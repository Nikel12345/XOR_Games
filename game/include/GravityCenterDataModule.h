#pragma once
#include <cstdint>
#include <vector>
#include "config.h"

class ObjectManager;
class BufferManager;
struct UploadTask;

struct GravityCenterGpu {
    float world_center[3];
    float padding;
};
static_assert(sizeof(GravityCenterGpu) == 16);

class GravityCenterDataModule {
public:
    void     Stamp(ObjectManager* object_manager, uint8_t slot);
    uint32_t Size(uint8_t slot) const;
    void     Store(BufferManager* buffer_manager, UploadTask* task, uint8_t slot) const;
    uint32_t AskCenterCount(uint8_t slot) const;

private:
    std::vector<GravityCenterGpu> centers[BUFFERING_LEVEL];
};
