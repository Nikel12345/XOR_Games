#pragma once

class EngineContext;
class GravityCenterDataModule;
class GravitationalLensDataModule;

namespace GameUpdateSet
{
    inline constexpr const char* GRAVITY_CENTERS_BUFFER    = "GravityCentersBuffer";
    inline constexpr const char* GRAVITATIONAL_LENS_BUFFER = "GravitationalLensBuffer";

    void SetGravityCentersUpdater(EngineContext* ctx, GravityCenterDataModule* gravity_center_data);
    void SetGravitationalLensUpdater(EngineContext* ctx, GravitationalLensDataModule* gravitational_lens_data);
}
