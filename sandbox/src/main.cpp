// ============================================================================
//  Sandbox: почему TDM store изолированно ~0.7 мс, а в профайлере движка 30+ мс?
//
//  Профайлер игры (при живом рендере) показал:
//    DefaultTransformBuffer .store  avg 43 ms   (буфер 12.5 МБ ≈ 200k энтити)
//    DefaultInstanceBuffer  .store  avg 1.5 ms  (тоже раздут → проблема системная)
//    gpu_frame 30 ms, fence_wait 25 ms           (GPU — бутылочное горло)
//  Изолированно тот же store = 18 GB/s. В движке = 0.29 GB/s. Разница НЕ в алгоритме.
//
//  Эксперимент: гоняем движково-репрезентативный store (реальный TDM через
//  CreateUpdateInstruction + bm->Execute*) в ДВУХ условиях —
//    (1) тихо;
//    (2) под фоновой нагрузкой на память (N-1 потоков стримят memcpy) + оверсабскрайб ядер.
//  На каждый store снимаем И wall-time, И реальные ТАКТЫ CPU потока (QueryThreadCycleTime):
//    такты те же, wall вырос  → sim-поток ВЫТЕСНЯЕТСЯ с ядра (оверсабскрайб);
//    такты тоже выросли       → поток на ядре, но СТОПОРИТСЯ о шину памяти.
//  Это отделяет «диспетчер отобрал ядро» от «упёрлись в память под контеншеном».
//
//  Рабочая директория — src/game (там saved_scene.scene).
// ============================================================================
#include <SDL3/SDL.h>
#define NOMINMAX               // иначе windows.h определяет max/min как макросы (ломает std::max)
#include <windows.h>            // QueryThreadCycleTime
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "TransferManager.h"
#include "BufferManager.h"
#include "DefaultResourceSet.h"
#include "BufferUpdateStruct.h"
#include "ObjectManager.h"
#include "TransformDataModule.h"
#include "ComponentSerializer.h"

using namespace DefaultBuffersNames;
using Clock = std::chrono::steady_clock;

static std::string ReadFileText(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf(); return ss.str();
}

// Фоновая нагрузка на память: каждый поток бесконечно memcpy'ит по приватным буферам
// (стрим ~64 МБ → мимо кэша, давит на контроллер памяти) и просто занимает ядро.
struct MemNoise {
    std::atomic<bool> run{false};
    std::vector<std::thread> ts;

    void start(unsigned n) {
        run.store(true, std::memory_order_release);
        for (unsigned i = 0; i < n; ++i)
            ts.emplace_back([this] {
                const size_t N = 32 * 1024 * 1024;   // 2x32МБ = 64МБ трафика на проход
                std::vector<char> a(N, 1), b(N, 2);
                while (run.load(std::memory_order_relaxed)) {
                    std::memcpy(a.data(), b.data(), N);
                    std::memcpy(b.data(), a.data(), N);
                }
            });
    }
    void stop() {
        run.store(false, std::memory_order_relaxed);
        for (auto& t : ts) t.join();
        ts.clear();
    }
};

int main(int argc, char** argv)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) { SDL_Log("SDL_Init: %s", SDL_GetError()); return 1; }
    SDL_Window* win = SDL_CreateWindow("TDM store contention", 320, 240, 0);
    SDL_GPUDevice* dev = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL, true, nullptr);
    if (!dev) { SDL_Log("CreateGPUDevice: %s", SDL_GetError()); return 1; }
    SDL_ClaimWindowForGPUDevice(dev, win);
    SDL_SetGPUAllowedFramesInFlight(dev, BUFFERING_LEVEL);

    TransferManager* tm  = new TransferManager(dev);
    BufferManager*   bm  = new BufferManager(dev, tm);
    DefaultResourceSet::CreateDefaultBuffers(bm);
    ObjectManager*   om  = new ObjectManager();
    TransformDataModule* tdm = new TransformDataModule();

    RegisterBuiltinComponentSpecs();
    SceneData* scene = om->CreateScene("main");
    const char* candidates[] = { argc > 1 ? argv[1] : "saved_scene.scene",
                                 "saved_scene.scene", "../game/saved_scene.scene",
                                 "src/game/saved_scene.scene" };
    std::string text; const char* used = nullptr;
    for (const char* p : candidates) { if (!p) continue; text = ReadFileText(p); if (!text.empty()) { used = p; break; } }
    if (text.empty()) { SDL_Log("Нет saved_scene.scene (cwd = src/game)."); return 1; }
    om->LoadScene("main", text);

    const uint32_t bytes = tdm->CalculateTransformSize(om, scene);
    const uint32_t num   = tdm->AskNumTransform(om, scene);
    const unsigned HW    = std::max(2u, std::thread::hardware_concurrency());
    SDL_Log("Сцена: %s   энтити: %u   буфер: %u байт (%.2f МБ)   ядер: %u",
            used, num, bytes, bytes / 1048576.0, HW);
    if (num == 0) { SDL_Log("Ноль энтити."); return 1; }

    const uint8_t slot = 0;
    bm->logic_index = slot;
    bm->CreateUpdateInstruction(DEFAULT_TRANSFORM_BUFFER,
        [om, tdm, scene](SDL_GPUCopyPass* cp, BufferManager* bmm, UploadTask& task)
    {
        tdm->StoreTransforms(bmm, &task, om, scene);
    },
        [om, tdm, scene]() -> uint32_t { return tdm->CalculateTransformSize(om, scene); });

    // Один «кадр»: снимаем wall (t1-t0) и такты потока вокруг Execute*. GPU-часть — вне.
    HANDLE hThread = GetCurrentThread();
    auto Frame = [&](double& wall_ms, double& mcycles) {
        ULONG64 c0 = 0, c1 = 0;
        auto t0 = Clock::now();
        QueryThreadCycleTime(hThread, &c0);
        SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
        SDL_GPUCopyPass*      cp = SDL_BeginGPUCopyPass(cb);
        TransferBufferData* tbd = bm->ExecuteUpdateInstructions(cp);
        bm->ExecuteUploadTasks(cp, slot);
        QueryThreadCycleTime(hThread, &c1);
        auto t1 = Clock::now();
        SDL_EndGPUCopyPass(cp);
        SDL_GPUFence* f = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
        SDL_WaitForGPUFences(dev, true, &f, 1);
        SDL_ReleaseGPUFence(dev, f);
        tm->ReleaseTB(tbd);
        wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        mcycles = (c1 - c0) / 1e6;
    };

    auto Measure = [&](const char* label, bool noisy) {
        MemNoise noise;
        if (noisy) noise.start(HW - 1);          // остальные ядра — под нагрузкой памяти
        const int warmup = 30, iters = 300;
        double w, m;
        for (int i = 0; i < warmup; ++i) Frame(w, m);
        std::vector<double> ws, ms; ws.reserve(iters); ms.reserve(iters);
        for (int i = 0; i < iters; ++i) { Frame(w, m); ws.push_back(w); ms.push_back(m); }
        if (noisy) noise.stop();
        std::sort(ws.begin(), ws.end());
        std::sort(ms.begin(), ms.end());
        const double wall = ws[ws.size()/2], mcyc = ms[ms.size()/2];
        // Такты→мс при 3.0 ГГц (база i5-7400): 1 Mcyc = 1e6/3e9 c = 1/3 мс.
        const double cpu_ms = mcyc / 3.0;
        SDL_Log("  %-22s wall med=%7.3f ms   такты med=%7.2f Mcyc (~%.3f ms CPU@3ГГц)   wall/cpu=%.2fx",
                label, wall, mcyc, cpu_ms, wall / cpu_ms);
    };

    SDL_Log("=== Store в двух условиях (wall vs реальные такты sim-потока) ===");
    Measure("тихо", false);
    Measure("под нагрузкой памяти", true);
    SDL_Log("Трактовка: такты ~те же, wall вырос → ВЫТЕСНЕНИЕ с ядра;");
    SDL_Log("           такты тоже выросли        → СТОПОР о шину памяти.");

    delete tdm; delete om; delete bm; delete tm;
    SDL_DestroyGPUDevice(dev); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
