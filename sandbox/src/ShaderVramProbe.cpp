#include "PCH.h"

#include <dxgi1_4.h>
#include <psapi.h>
#include <filesystem>
#include <map>
#include <vector>
#include <string>

#pragma comment(lib, "dxgi.lib")

/*
    Зонд: во что реально обходится ДЕРЖАТЬ шейдер резидентным — модуль (SDL_GPUShader) и
    собранный из него пайплайн.

    Вопрос прикладной: если вынести sp/vs/fs из манифестов сцены в дефолтные ресурсы, они будут
    создаваться ВСЕГДА и для КАЖДОЙ сцены, использует она их или нет — а PipeManager собирает
    пайплайн на каждую sp в реестре (CreateGraphicsPiplenes идёт по всему словарю, без учёта
    того, ссылается ли на sp хоть один батч). То есть цена «дефолтного» шейдера = модуль + пайплайн.

    Меряем ДЕЛЬТАМИ на пачках по BATCH штук: одиночный объект тонет в шуме драйверных суб-аллокаций
    (драйвер берёт память кусками по мегабайту и режет внутри). Дедуп ShaderManager по хэшу SPIR-V
    здесь обошли намеренно — зовём SDL_ShaderCross_* напрямую, иначе N копий схлопнулись бы в одну.

    Мерная лента — DXGI QueryVideoMemoryInfo: WDDM ведёт учёт ПО ПРОЦЕССУ, поэтому дельта — наша,
    а не общая по системе. LOCAL = собственно видеопамять, NON_LOCAL = системная, отданная GPU
    (драйвер держит код шейдеров то там, то там — смотрим обе). Рядом рабочий набор процесса:
    часть цены шейдера — это CPU-side копия байткода в драйвере, и она не в VRAM.

    Запуск: рабочая директория src/game (см. CMakeLists песочницы) — оттуда берётся кэш .spv
    игры, компилировать HLSL заново незачем.
*/

namespace {

struct Sample {
    uint64_t local = 0;      // DXGI_MEMORY_SEGMENT_GROUP_LOCAL, CurrentUsage этого процесса
    uint64_t nonlocal = 0;   // DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL
    uint64_t working_set = 0;
};

IDXGIAdapter3* g_adapter = nullptr;

bool InitVramMeter()
{
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&factory))) return false;

    // Адаптер 0 — дискретный на этой машине; зонду хватает, полноценный матчинг с SDL-девайсом
    // потребовал бы LUID, которого SDL_GPU наружу не отдаёт.
    IDXGIAdapter1* a1 = nullptr;
    bool ok = false;
    if (SUCCEEDED(factory->EnumAdapters1(0, &a1))) {
        DXGI_ADAPTER_DESC1 desc{};
        a1->GetDesc1(&desc);
        SDL_Log("VRAM meter: adapter[0] dedicated=%llu MB",
            (unsigned long long)(desc.DedicatedVideoMemory >> 20));
        ok = SUCCEEDED(a1->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&g_adapter));
        a1->Release();
    }
    factory->Release();
    return ok;
}

Sample Measure()
{
    Sample s{};
    if (g_adapter) {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (SUCCEEDED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
            s.local = info.CurrentUsage;
        if (SUCCEEDED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &info)))
            s.nonlocal = info.CurrentUsage;
    }
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        s.working_set = pmc.WorkingSetSize;
    return s;
}

void Report(const char* what, const Sample& a, const Sample& b, int count, double ms = -1.0, double first_ms = -1.0)
{
    const int64_t dl = (int64_t)b.local - (int64_t)a.local;
    const int64_t dn = (int64_t)b.nonlocal - (int64_t)a.nonlocal;
    const int64_t dw = (int64_t)b.working_set - (int64_t)a.working_set;
    // Драйвер кэширует скомпилированный код по хэшу SPIR-V, поэтому копии 2..N почти бесплатны.
    // Настоящая цена «ещё одного шейдера» — ПЕРВАЯ, холодная сборка; её и печатаем отдельно.
    char t[64] = "";
    if (ms >= 0.0) SDL_snprintf(t, sizeof(t), " | first %6.2f ms, rest %5.3f ms", first_ms, (ms - first_ms) / (count - 1));
    SDL_Log("  %-34s x%-4d | VRAM local %+8.1f KB (%6.2f KB each) | non-local %+8.1f KB (%6.2f each) | RAM %+8.1f KB (%6.2f each)%s",
        what, count,
        dl / 1024.0, dl / 1024.0 / count,
        dn / 1024.0, dn / 1024.0 / count,
        dw / 1024.0, dw / 1024.0 / count, t);
}

// ── Загрузка .spv из кэша игры ────────────────────────────────────────────────────────────────
struct Spv {
    std::string name;
    std::vector<Uint8> bytes;
};

// Кэш лежит рядом с exe игры (SDL_GetBasePath + shaders/shader_cache). У зонда base свой, поэтому
// идём от рабочей директории (src/game) в build. Имя файла = <исходник>.<хэш>.spv, берём самый
// свежий на исходник: старые варианты — протухшие ключи кэша, размер у них того же порядка.
std::vector<Spv> LoadCachedSpv()
{
    namespace fs = std::filesystem;
    std::vector<Spv> out;

    for (const char* cfg : { "Release", "Debug" }) {
        const fs::path dir = fs::path("../../build/src/game") / cfg / "shaders/shader_cache";
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;

        std::map<std::string, fs::path> newest;   // исходник -> самый свежий .spv
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file() || e.path().extension() != ".spv") continue;
            std::string fn = e.path().filename().string();
            const size_t dot = fn.find(".hlsl.");
            if (dot == std::string::npos) continue;
            const std::string src = fn.substr(0, dot);
            auto it = newest.find(src);
            if (it == newest.end() || fs::last_write_time(e.path()) > fs::last_write_time(it->second))
                newest[src] = e.path();
        }
        for (auto& [src, p] : newest) {
            Spv s;
            s.name = src;
            const uintmax_t sz = fs::file_size(p, ec);
            s.bytes.resize((size_t)sz);
            if (FILE* f = fopen(p.string().c_str(), "rb")) {
                fread(s.bytes.data(), 1, s.bytes.size(), f);
                fclose(f);
                out.push_back(std::move(s));
            }
        }
        if (!out.empty()) { SDL_Log("SPIR-V cache: %s (%zu shaders)", dir.string().c_str(), out.size()); break; }
    }
    return out;
}

}   // namespace

void RunShaderVramProbe()
{
    if (!InitVramMeter()) { SDL_Log("DXGI meter unavailable - aborting"); return; }

    // SPIRV-ONLY: на 3.4 запрос «любой формат» уводит в D3D12, и compute-пайплайны из сырого
    // SPIR-V там не собираются (см. SDL_FORK.md / заметку про 3.4.14).
    SDL_Window* win = SDL_CreateWindow("shader vram probe", 320, 240, 0);
    SDL_GPUDevice* dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr);
    if (!dev || !win) { SDL_Log("device/window creation failed: %s", SDL_GetError()); return; }
    SDL_ClaimWindowForGPUDevice(dev, win);
    SDL_ShaderCross_Init();

    SDL_Log("backend = %s", SDL_GetGPUDeviceDriver(dev));

    std::vector<Spv> spvs = LoadCachedSpv();
    if (spvs.empty()) { SDL_Log("no .spv found - run the game once to populate the cache"); return; }

    const SDL_GPUTextureFormat color_fmt = SDL_GetGPUSwapchainTextureFormat(dev, win);

    // Пачка на замер: драйвер отдаёт память страницами, одиночный объект дельты не даёт.
    constexpr int BATCH = 256;

    // Прогрев: первый шейдер тянет за собой ленивую инициализацию драйвера (компилятор, кучи) —
    // без него она вся легла бы в первую же измеряемую дельту.
    {
        SDL_ShaderCross_GraphicsShaderMetadata* md =
            SDL_ShaderCross_ReflectGraphicsSPIRV(spvs[0].bytes.data(), spvs[0].bytes.size(), 0);
        if (md) SDL_free(md);
    }

    SDL_Log("");
    SDL_Log("=== per-shader cost (batch of %d, cost per ONE) ===", BATCH);

    for (const Spv& s : spvs) {
        // Стадию берём из имени исходника: .comp -> compute, .vert -> vertex, иначе fragment.
        const bool is_comp = s.name.find(".comp") != std::string::npos;
        const bool is_vert = s.name.find(".vert") != std::string::npos;

        if (is_comp) {
            SDL_ShaderCross_ComputePipelineMetadata* md =
                SDL_ShaderCross_ReflectComputeSPIRV(s.bytes.data(), s.bytes.size(), 0);
            if (!md) { SDL_Log("  %-24s reflect FAILED", s.name.c_str()); continue; }

            SDL_ShaderCross_SPIRV_Info info{};
            info.bytecode = s.bytes.data(); info.bytecode_size = s.bytes.size();
            info.entrypoint = "main"; info.shader_stage = SDL_SHADERCROSS_SHADERSTAGE_COMPUTE;

            std::vector<SDL_GPUComputePipeline*> pipes;
            pipes.reserve(BATCH);
            const Sample a = Measure();
            const Uint64 t0 = SDL_GetPerformanceCounter();
            double first_ms = 0.0;
            for (int i = 0; i < BATCH; ++i) {
                if (SDL_GPUComputePipeline* p = SDL_ShaderCross_CompileComputePipelineFromSPIRV(dev, &info, md, 0))
                    pipes.push_back(p);
                if (i == 0) first_ms = (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
            }
            const double ms = (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
            const Sample b = Measure();

            char label[128];
            SDL_snprintf(label, sizeof(label), "%s [%zu B spv] compute pipe", s.name.c_str(), s.bytes.size());
            Report(label, a, b, (int)pipes.size(), ms, first_ms);

            for (auto* p : pipes) SDL_ReleaseGPUComputePipeline(dev, p);
            SDL_free(md);
            continue;
        }

        SDL_ShaderCross_GraphicsShaderMetadata* md =
            SDL_ShaderCross_ReflectGraphicsSPIRV(s.bytes.data(), s.bytes.size(), 0);
        if (!md) { SDL_Log("  %-24s reflect FAILED", s.name.c_str()); continue; }

        SDL_ShaderCross_SPIRV_Info info{};
        info.bytecode = s.bytes.data(); info.bytecode_size = s.bytes.size();
        info.entrypoint = "main";
        info.shader_stage = is_vert ? SDL_SHADERCROSS_SHADERSTAGE_VERTEX : SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT;

        // (1) МОДУЛЬ — то, чем владеет ShaderData: SPIR-V, отданный драйверу, но ещё не скомпилированный
        //     в машинный код. Именно он остаётся, если sp на шейдер не ссылается.
        std::vector<SDL_GPUShader*> mods;
        mods.reserve(BATCH);
        const Sample a = Measure();
        const Uint64 t0 = SDL_GetPerformanceCounter();
        double first_ms = 0.0;
        for (int i = 0; i < BATCH; ++i) {
            if (SDL_GPUShader* sh = SDL_ShaderCross_CompileGraphicsShaderFromSPIRV(dev, &info, &md->resource_info, 0))
                mods.push_back(sh);
            if (i == 0) first_ms = (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
        }
        const double ms = (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
        const Sample b = Measure();

        char label[128];
        SDL_snprintf(label, sizeof(label), "%s [%zu B spv] module", s.name.c_str(), s.bytes.size());
        Report(label, a, b, (int)mods.size(), ms, first_ms);

        for (auto* sh : mods) SDL_ReleaseGPUShader(dev, sh);
        SDL_free(md);
    }

    // (2) ПАЙПЛАЙН — то, что PipeManager строит на КАЖДУЮ sp в реестре. Здесь драйвер и компилирует
    //     SPIR-V в ISA, так что это и есть настоящая цена «лишней» sp. Берём реальную пару из игры.
    SDL_Log("");
    SDL_Log("=== graphics pipeline cost (batch of %d) ===", BATCH);
    {
        const Spv* vs = nullptr; const Spv* fs = nullptr;
        for (const Spv& s : spvs) {
            if (!vs && s.name == "main_pass.vert") vs = &s;
            if (!fs && s.name == "surface")        fs = &s;
        }
        if (!vs || !fs) { for (const Spv& s : spvs) { if (!vs && s.name.find(".vert") != std::string::npos) vs = &s; else if (!fs && s.name.find(".comp") == std::string::npos && s.name.find(".vert") == std::string::npos) fs = &s; } }

        if (vs && fs) {
            auto* vmd = SDL_ShaderCross_ReflectGraphicsSPIRV(vs->bytes.data(), vs->bytes.size(), 0);
            auto* fmd = SDL_ShaderCross_ReflectGraphicsSPIRV(fs->bytes.data(), fs->bytes.size(), 0);

            SDL_ShaderCross_SPIRV_Info vi{};
            vi.bytecode = vs->bytes.data(); vi.bytecode_size = vs->bytes.size();
            vi.entrypoint = "main"; vi.shader_stage = SDL_SHADERCROSS_SHADERSTAGE_VERTEX;
            SDL_ShaderCross_SPIRV_Info fi{};
            fi.bytecode = fs->bytes.data(); fi.bytecode_size = fs->bytes.size();
            fi.entrypoint = "main"; fi.shader_stage = SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT;

            SDL_GPUShader* vsh = SDL_ShaderCross_CompileGraphicsShaderFromSPIRV(dev, &vi, &vmd->resource_info, 0);
            SDL_GPUShader* fsh = SDL_ShaderCross_CompileGraphicsShaderFromSPIRV(dev, &fi, &fmd->resource_info, 0);

            if (vsh && fsh) {
                SDL_GPUColorTargetDescription ct{};
                ct.format = color_fmt;

                SDL_GPUGraphicsPipelineCreateInfo pci{};
                pci.vertex_shader = vsh;
                pci.fragment_shader = fsh;
                pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
                pci.target_info.num_color_targets = 1;
                pci.target_info.color_target_descriptions = &ct;
                // vertex_input_state пуст: зонд ничего не рисует, а компиляцию ISA это не отменяет.

                std::vector<SDL_GPUGraphicsPipeline*> pipes;
                pipes.reserve(BATCH);
                const Sample a = Measure();
                const Uint64 t0 = SDL_GetPerformanceCounter();
                double first_ms = 0.0;
                for (int i = 0; i < BATCH; ++i) {
                    if (SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(dev, &pci))
                        pipes.push_back(p);
                    if (i == 0) first_ms = (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
                }
                const double ms = (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
                const Sample b = Measure();

                char label[128];
                SDL_snprintf(label, sizeof(label), "%s + %s pipeline", vs->name.c_str(), fs->name.c_str());
                Report(label, a, b, (int)pipes.size(), ms, first_ms);
                if (pipes.empty()) SDL_Log("    (pipeline creation failed: %s)", SDL_GetError());

                for (auto* p : pipes) SDL_ReleaseGPUGraphicsPipeline(dev, p);
            }
            else SDL_Log("  shader module creation failed: %s", SDL_GetError());

            if (vsh) SDL_ReleaseGPUShader(dev, vsh);
            if (fsh) SDL_ReleaseGPUShader(dev, fsh);
            SDL_free(vmd); SDL_free(fmd);
        }
    }

    // КОНТРОЛЬ прибора: заведомо крупная аллокация в VRAM. Без неё нули выше не доказывают
    // ничего — с тем же успехом это мог бы быть неработающий счётчик.
    SDL_Log("");
    SDL_Log("=== control: does the meter move at all? ===");
    {
        SDL_GPUTextureCreateInfo tci{};
        tci.type = SDL_GPU_TEXTURETYPE_2D;
        tci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        tci.width = 1024; tci.height = 1024; tci.layer_count_or_depth = 1; tci.num_levels = 1;
        tci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;   // 4 MB каждая

        std::vector<SDL_GPUTexture*> texs;
        const Sample a = Measure();
        for (int i = 0; i < 16; ++i)
            if (SDL_GPUTexture* t = SDL_CreateGPUTexture(dev, &tci)) texs.push_back(t);
        const Sample b = Measure();
        Report("1024x1024 RGBA8 texture (4 MB)", a, b, (int)texs.size());
        for (auto* t : texs) SDL_ReleaseGPUTexture(dev, t);
    }

    // Итог для решения: сколько стоит ВЕСЬ текущий набор, если держать его резидентным всегда.
    SDL_Log("");
    size_t total_spv = 0;
    for (const Spv& s : spvs) total_spv += s.bytes.size();
    SDL_Log("total SPIR-V of the whole current set: %zu shaders, %.1f KB", spvs.size(), total_spv / 1024.0);

    SDL_ShaderCross_Quit();
    SDL_ReleaseWindowFromGPUDevice(dev, win);
    SDL_DestroyGPUDevice(dev);
    SDL_DestroyWindow(win);
    if (g_adapter) g_adapter->Release();
}

int main(int, char**)
{
    SDL_Init(SDL_INIT_VIDEO);
    RunShaderVramProbe();
    SDL_Quit();
    return 0;
}
