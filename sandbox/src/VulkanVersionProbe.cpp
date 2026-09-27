#include "PCH.h"

/*
    Зонд: отсечение устройств по объявленной версии Vulkan (ENGINE-FORK в
    VULKAN_INTERNAL_DeterminePhysicalDevice).

    Три прогона без окна: без SDL_GPUVulkanOptions (штатные 1.0 SDL — проверка не включается),
    с 1.3 (минимум движка — устройство обязано пройти) и с заведомо недостижимой 1.9 (обязано
    отсечься, в логе — строка форка с именем карты и её версией, SDL_CreateGPUDevice = NULL).
*/

static void Probe(const char* label, const SDL_GPUVulkanOptions* opts)
{
    SDL_Log("=== %s ===", label);
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
    if (opts)
        SDL_SetPointerProperty(props, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER, const_cast<SDL_GPUVulkanOptions*>(opts));
    SDL_GPUDevice* dev = SDL_CreateGPUDeviceWithProperties(props);
    SDL_DestroyProperties(props);

    if (!dev) {
        SDL_Log("  device: NULL (%s)", SDL_GetError());
        return;
    }
    const SDL_PropertiesID info = SDL_GetGPUDeviceProperties(dev);
    SDL_Log("  device: %s / %s, driver %s", SDL_GetGPUDeviceDriver(dev),
        SDL_GetStringProperty(info, SDL_PROP_GPU_DEVICE_NAME_STRING, "?"),
        SDL_GetStringProperty(info, SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING, "?"));
    SDL_DestroyGPUDevice(dev);
}

int main(int, char**)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) { SDL_Log("SDL_Init failed: %s", SDL_GetError()); return 1; }

    Probe("default (no Vulkan options)", nullptr);

    SDL_GPUVulkanOptions v13{};
    v13.vulkan_api_version = (1u << 22) | (3u << 12);
    Probe("Vulkan 1.3", &v13);

    SDL_GPUVulkanOptions v19{};
    v19.vulkan_api_version = (1u << 22) | (9u << 12);
    Probe("Vulkan 1.9 (must be rejected)", &v19);

    SDL_Quit();
    return 0;
}
