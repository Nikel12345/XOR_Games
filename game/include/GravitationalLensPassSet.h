#pragma once
#include <cstdint>

class EngineContext;
class GravitationalLensDataModule;

namespace GravitationalLensPassSet
{
    inline constexpr const char* LENS_PASS = "GravitationalLensPass";

    void SetPasses(EngineContext* ctx, uint32_t base_width, uint32_t base_height,
                   GravitationalLensDataModule* gravitational_lens_data);
}
