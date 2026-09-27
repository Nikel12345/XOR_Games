#include "PCH.h"

/*
    Зонд: почему на SDL 3.4.14 окно не заклеймливается GPU-девайсом (регрессия против 3.2.4).

    Симптом в игре: SDL_ClaimWindowForGPUDevice возвращает ИСТИНУ, а все последующие запросы
    свопчейна отвечают «Must claim window before…». То есть отказ не сигнализируется возвратом —
    поэтому проверка на месте вызова (мы её добавили) поломку не ловит.

    Зонд отделяет это от движка: голые SDL-вызовы, ничего своего. Перебираем бэкенд × debug_mode,
    потому что оба подозреваемых: выбор бэкенда на 3.4 сменился на D3D12, а под Vulkan в логе
    ещё и «Validation layers not found» — надо знать, влияет ли debug на claim.

    Критерий — НЕ возврат claim, а ответ SDL_WindowSupportsGPUPresentMode/GetSwapchainTextureFormat
    после него: именно они показывают, зарегистрировалось ли окно на самом деле.
*/

static void Probe(const char* driver, bool debug_mode, SDL_GPUShaderFormat formats, const char* fmt_label,
    SDL_WindowFlags win_flags = SDL_WINDOW_RESIZABLE, const char* flags_label = "RESIZABLE")
{
    SDL_Log("=== driver=%s debug=%s formats=%s window=%s ===",
        driver ? driver : "(auto)", debug_mode ? "true" : "false", fmt_label, flags_label);

    SDL_Window* win = SDL_CreateWindow("claim probe", 320, 240, win_flags);
    if (!win) { SDL_Log("  CreateWindow FAILED: %s", SDL_GetError()); return; }

    SDL_GPUDevice* dev = SDL_CreateGPUDevice(formats, debug_mode, driver);
    if (!dev) {
        SDL_Log("  CreateGPUDevice FAILED: %s", SDL_GetError());
        SDL_DestroyWindow(win);
        return;
    }
    SDL_Log("  backend            = %s", SDL_GetGPUDeviceDriver(dev));
    SDL_Log("  shader formats     = 0x%08X", (unsigned)SDL_GetGPUShaderFormats(dev));

    SDL_ClearError();
    const bool claimed = SDL_ClaimWindowForGPUDevice(dev, win);
    const char* claim_err = SDL_GetError();
    SDL_Log("  ClaimWindow        -> %s   (SDL_GetError: '%s')",
        claimed ? "true" : "FALSE", claim_err && *claim_err ? claim_err : "");

    // Единственный вызов, стоящий в игре МЕЖДУ claim и запросами свопчейна. Подозреваемый:
    // если он пересоздаёт свопчейн и на каком-то бэкенде падает, окно останется незаклеймленным,
    // а claim выше уже отчитался успехом — ровно наблюдаемая картина.
    SDL_ClearError();
    const bool fif = SDL_SetGPUAllowedFramesInFlight(dev, 3);
    const char* fif_err = SDL_GetError();
    SDL_Log("  AllowedFramesInFlight(3) -> %s   (SDL_GetError: '%s')",
        fif ? "true" : "FALSE", fif_err && *fif_err ? fif_err : "");

    // Настоящая проверка: верит ли сам SDL, что окно заклеймлено.
    SDL_ClearError();
    const bool supports = SDL_WindowSupportsGPUPresentMode(dev, win, SDL_GPU_PRESENTMODE_VSYNC);
    const char* sup_err = SDL_GetError();
    SDL_Log("  SupportsVSYNC      -> %s   (SDL_GetError: '%s')",
        supports ? "true" : "false", sup_err && *sup_err ? sup_err : "");

    SDL_ClearError();
    const SDL_GPUTextureFormat fmt = SDL_GetGPUSwapchainTextureFormat(dev, win);
    const char* fmt_err = SDL_GetError();
    SDL_Log("  SwapchainFormat    -> %d %s (SDL_GetError: '%s')",
        (int)fmt, fmt == SDL_GPU_TEXTUREFORMAT_INVALID ? "(INVALID)" : "(ok)",
        fmt_err && *fmt_err ? fmt_err : "");

    // Реальная выдача кадра — финальный критерий: свопчейн живой или нет.
    if (SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev)) {
        SDL_GPUTexture* tex = nullptr;
        Uint32 w = 0, h = 0;
        SDL_ClearError();
        const bool got = SDL_WaitAndAcquireGPUSwapchainTexture(cb, win, &tex, &w, &h);
        const char* acq_err = SDL_GetError();
        SDL_Log("  AcquireSwapchain   -> %s tex=%p %ux%u (SDL_GetError: '%s')",
            got ? "true" : "false", (void*)tex, w, h, acq_err && *acq_err ? acq_err : "");
        SDL_SubmitGPUCommandBuffer(cb);
    }

    SDL_ReleaseWindowFromGPUDevice(dev, win);
    SDL_DestroyGPUDevice(dev);
    SDL_DestroyWindow(win);
    SDL_Log("");
}

// Проверка обхода в той форме, в какой он нужен игре: ОДНО окно, а перед боевым девайсом
// создаётся и сразу уничтожается «прогревочный». Если claim после этого проходит — обход годен
// (иначе особенным будет не первый девайс, а первое окно, и лечить придётся иначе).
// ASCII-only в логах: файл сохранён как UTF-8, а MSVC читает его как CP1251 — кириллица в
// строковых литералах выходит кашей (та же причина, что в EngineProfiler.cpp).
static void ProbeWarmup(bool warm_window)
{
    SDL_Log("=== workaround: warm up %s, then real window+device ===",
        warm_window ? "WINDOW" : "DEVICE");

    // Прогрев ОКНА: создаём и сразу уничтожаем окно-пустышку ДО боевого.
    if (warm_window) {
        if (SDL_Window* w = SDL_CreateWindow("warm", 64, 64, 0)) {
            SDL_DestroyWindow(w);
            SDL_Log("  warmup window created and destroyed");
        }
    }

    SDL_Window* win = SDL_CreateWindow("claim probe", 320, 240, SDL_WINDOW_RESIZABLE);
    if (!win) { SDL_Log("  CreateWindow FAILED: %s", SDL_GetError()); return; }

    if (!warm_window) {
        if (SDL_GPUDevice* warm = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, "vulkan")) {
            SDL_DestroyGPUDevice(warm);
            SDL_Log("  warmup device created and destroyed");
        }
    }

    SDL_GPUDevice* dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, "vulkan");
    if (!dev) { SDL_Log("  real device FAILED: %s", SDL_GetError()); SDL_DestroyWindow(win); return; }

    SDL_Log("  ClaimWindow        -> %s", SDL_ClaimWindowForGPUDevice(dev, win) ? "true" : "FALSE");
    SDL_ClearError();
    const SDL_GPUTextureFormat fmt = SDL_GetGPUSwapchainTextureFormat(dev, win);
    SDL_Log("  SwapchainFormat    -> %d %s (SDL_GetError: '%s')",
        (int)fmt, fmt == SDL_GPU_TEXTUREFORMAT_INVALID ? "(INVALID)" : "(ok)", SDL_GetError());

    SDL_ReleaseWindowFromGPUDevice(dev, win);
    SDL_DestroyGPUDevice(dev);
    SDL_DestroyWindow(win);
    SDL_Log("");
}

int main()
{
    if (!SDL_Init(SDL_INIT_VIDEO)) { SDL_Log("SDL_Init: %s", SDL_GetError()); return 1; }

    SDL_Log("SDL compiled %d.%d.%d / linked %d.%d.%d",
        SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION,
        SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
        SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
        SDL_VERSIONNUM_MICRO(SDL_GetVersion()));
    SDL_Log("");

    const SDL_GPUShaderFormat ALL = SDL_GPU_SHADERFORMAT_SPIRV
                                  | SDL_GPU_SHADERFORMAT_DXIL
                                  | SDL_GPU_SHADERFORMAT_MSL;
    const SDL_GPUShaderFormat SPV = SDL_GPU_SHADERFORMAT_SPIRV;

    // ПОРЯДОК — подозреваемый. Раньше vulkan-кейсы шли ВТОРЫМИ и проходили, а первый (тоже vulkan)
    // ломался; игра же создаёт РОВНО ОДИН девайс, то есть всегда «первый». Гипотеза: не срабатывает
    // claim у ПЕРВОГО в процессе Vulkan-девайса, а не у какого-то способа его выбора.
    // Поэтому здесь один и тот же кейс подряд: если первый упадёт, а второй пройдёт — гипотеза верна.
    (void)ALL;
    // Кандидат в лечение: окно, СОЗДАННОЕ под Vulkan. Тогда лоадер и поверхность готовы к моменту
    // claim, и «первый девайс в процессе» перестаёт быть особенным. Кейс идёт ПЕРВЫМ — иначе
    // проверка бессмысленна (второй Vulkan-девайс работает и без флага).
    ProbeWarmup(/*warm_window=*/true);
    Probe("vulkan", true, SPV, "SPIRV");

    SDL_Quit();
    return 0;
}
