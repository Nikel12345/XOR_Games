// ============================================================================
//  Зонд: во что обходятся ПУСТЫЕ команды (num_instances = 0) в indirect-мультидроу.
//
//  Вопрос из плана LOD (plans/lod.md): команды невыбранных уровней лежат в буфере с нулём
//  инстансов. Если GPU платит за каждую такую команду заметно, раскладка «все уровни всегда
//  в буфере» становится дорогой, и число команд придётся оптимизировать.
//
//  Один вызов SDL_DrawGPUIndexedPrimitivesIndirect на N команд (как движковый мультидроу
//  текстурной группы; SDL на Vulkan зовёт настоящий vkCmdDrawIndexedIndirect с drawCount).
//  Раскладки буфера (драйвер может вести себя по-разному в зависимости от плотности):
//    empty   — все команды пустые;
//    one     — все пустые, одна настоящая в середине;
//    checker — через одну: настоящая / пустая;
//    full    — все настоящие;
//    full/2  — N/2 настоящих подряд: вычитается из checker, остаток = цена N/2 пустых
//              вперемешку с настоящими.
//  Настоящая команда = 1 инстанс крошечного треугольника (3 индекса, несколько пикселей),
//  чтобы замер был про обработку команд, а не про растеризацию.
//
//  Для сравнения каждая раскладка меряется и «без draw_count»: N вызовов по одной команде
//  (offset = i * sizeof(команды), draw_count = 1) — так, как SDL рисует сам на устройстве без
//  multiDrawIndirect. Отдельно печатается время ЗАПИСИ cb на CPU: у N вызовов оно своё.
//
//  Время = от сабмита cb до сигнала fence (CPU-часы), медиана и минимум по REPEATS прогонам.
//  В него входят задержки сабмита/очереди — они снимаются базой: cb с тем же рендер-проходом
//  без дроу. Валидация выключена: она меняет время.
//
//  Рабочая директория не важна: шейдеры компилируются из строк ниже.
// ============================================================================
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3_shadercross/SDL_shadercross.h>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

constexpr int      REPEATS = 41;
constexpr int      WARMUP  = 5;
constexpr uint32_t TARGET  = 256;
constexpr uint32_t CMD_BYTES = sizeof(SDL_GPUIndexedIndirectDrawCommand);
constexpr uint32_t COUNTS[] = { 1'000, 10'000, 100'000, 1'000'000 };
constexpr uint32_t MAX_CMDS = 1'000'000;

const char* kVS = R"(
struct VSOut { float4 pos : SV_Position; };
VSOut main(uint vid : SV_VertexID)
{
    // Треугольник в пару пикселей в углу таргета: растеризация не должна влиять на замер.
    float2 v = (vid == 0u) ? float2(-0.99, -0.99) : ((vid == 1u) ? float2(-0.98, -0.99) : float2(-0.99, -0.98));
    VSOut o; o.pos = float4(v, 0.0, 1.0); return o;
}
)";

const char* kFS = R"(
float4 main(float4 pos : SV_Position) : SV_Target0 { return float4(1.0, 0.0, 0.0, 1.0); }
)";

SDL_GPUShader* Compile(SDL_GPUDevice* dev, const char* src, SDL_ShaderCross_ShaderStage stage)
{
    SDL_ShaderCross_HLSL_Info hi{};
    hi.source = src;
    hi.entrypoint = "main";
    hi.shader_stage = stage;
    size_t size = 0;
    void* spv = SDL_ShaderCross_CompileSPIRVFromHLSL(&hi, &size);
    if (!spv) { SDL_Log("HLSL->SPIRV failed: %s", SDL_GetError()); return nullptr; }

    SDL_ShaderCross_GraphicsShaderMetadata* md =
        SDL_ShaderCross_ReflectGraphicsSPIRV(static_cast<const Uint8*>(spv), size, 0);
    SDL_ShaderCross_SPIRV_Info si{};
    si.bytecode = static_cast<const Uint8*>(spv);
    si.bytecode_size = size;
    si.entrypoint = "main";
    si.shader_stage = stage;
    SDL_GPUShader* sh = md ? SDL_ShaderCross_CompileGraphicsShaderFromSPIRV(dev, &si, &md->resource_info, 0) : nullptr;
    if (!sh) SDL_Log("SPIRV->shader failed: %s", SDL_GetError());
    SDL_free(md);
    SDL_free(spv);
    return sh;
}

enum class Layout { Empty, One, Checker, Full, HalfFull };

const char* Name(Layout l)
{
    switch (l) {
    case Layout::Empty:    return "empty";
    case Layout::One:      return "one";
    case Layout::Checker:  return "checker";
    case Layout::Full:     return "full";
    case Layout::HalfFull: return "full/2";
    }
    return "?";
}

struct Ctx {
    SDL_GPUDevice*           dev = nullptr;
    SDL_GPUGraphicsPipeline* pipe = nullptr;
    SDL_GPUTexture*          target = nullptr;
    SDL_GPUBuffer*           index = nullptr;
    SDL_GPUBuffer*           indirect = nullptr;
    SDL_GPUTransferBuffer*   staging = nullptr;
};

void SubmitAndWait(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cb)
{
    SDL_GPUFence* f = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
    SDL_WaitForGPUFences(dev, true, &f, 1);
    SDL_ReleaseGPUFence(dev, f);
}

// Заливка (вне замера): команды раскладки, copy pass, ожидание fence. Возвращает draw_count.
uint32_t Fill(Ctx& c, Layout layout, uint32_t n)
{
    const uint32_t draw_count = (layout == Layout::HalfFull) ? n / 2 : n;
    auto* cmds = static_cast<SDL_GPUIndexedIndirectDrawCommand*>(SDL_MapGPUTransferBuffer(c.dev, c.staging, true));
    if (!cmds) { SDL_Log("map failed: %s", SDL_GetError()); return 0; }
    for (uint32_t i = 0; i < draw_count; ++i) {
        bool real = false;
        switch (layout) {
        case Layout::Empty:    real = false; break;
        case Layout::One:      real = (i == draw_count / 2); break;
        case Layout::Checker:  real = (i % 2 == 0); break;
        case Layout::Full:
        case Layout::HalfFull: real = true; break;
        }
        cmds[i].num_indices    = 3;
        cmds[i].num_instances  = real ? 1u : 0u;
        cmds[i].first_index    = 0;
        cmds[i].vertex_offset  = 0;
        cmds[i].first_instance = i;   // как в движке: у каждой команды свой адрес
    }
    SDL_UnmapGPUTransferBuffer(c.dev, c.staging);

    SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(c.dev);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
    SDL_GPUTransferBufferLocation src{ c.staging, 0 };
    SDL_GPUBufferRegion dst{ c.indirect, 0, draw_count * CMD_BYTES };
    SDL_UploadToGPUBuffer(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    SubmitAndWait(c.dev, cb);
    return draw_count;
}

// Один прогон: рендер-проход, мультидроу на draw_count команд (0 = проход без дроу) либо
// draw_count вызовов по одной команде (split). Возвращает время сабмит->fence, мс; время
// записи cb кладёт в encode_ms.
double RunOnce(Ctx& c, uint32_t draw_count, bool split, double* encode_ms = nullptr)
{
    const Uint64 e0 = SDL_GetPerformanceCounter();
    SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(c.dev);
    SDL_GPUColorTargetInfo ct{};
    ct.texture = c.target;
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cb, &ct, 1, nullptr);
    if (draw_count > 0) {
        SDL_BindGPUGraphicsPipeline(rp, c.pipe);
        SDL_GPUBufferBinding ib{ c.index, 0 };
        SDL_BindGPUIndexBuffer(rp, &ib, SDL_GPU_INDEXELEMENTSIZE_16BIT);
        if (split)
            for (uint32_t i = 0; i < draw_count; ++i)
                SDL_DrawGPUIndexedPrimitivesIndirect(rp, c.indirect, i * CMD_BYTES, 1);
        else
            SDL_DrawGPUIndexedPrimitivesIndirect(rp, c.indirect, 0, draw_count);
    }
    SDL_EndGPURenderPass(rp);
    if (encode_ms)
        *encode_ms = double(SDL_GetPerformanceCounter() - e0) * 1000.0 / double(SDL_GetPerformanceFrequency());

    const Uint64 t0 = SDL_GetPerformanceCounter();
    SubmitAndWait(c.dev, cb);
    const Uint64 t1 = SDL_GetPerformanceCounter();
    return double(t1 - t0) * 1000.0 / double(SDL_GetPerformanceFrequency());
}

struct Stat { double median = 0, min = 0, encode = 0; };

Stat Measure(Ctx& c, uint32_t draw_count, bool split = false)
{
    for (int i = 0; i < WARMUP; ++i) RunOnce(c, draw_count, split);
    std::vector<double> t, e;
    t.reserve(REPEATS);
    e.reserve(REPEATS);
    for (int i = 0; i < REPEATS; ++i) {
        double enc = 0.0;
        t.push_back(RunOnce(c, draw_count, split, &enc));
        e.push_back(enc);
    }
    std::sort(t.begin(), t.end());
    std::sort(e.begin(), e.end());
    return { t[t.size() / 2], t.front(), e[e.size() / 2] };
}

bool Setup(Ctx& c)
{
    c.dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr);
    if (!c.dev) { SDL_Log("CreateGPUDevice: %s", SDL_GetError()); return false; }
    SDL_Log("backend = %s", SDL_GetGPUDeviceDriver(c.dev));

    SDL_GPUShader* vs = Compile(c.dev, kVS, SDL_SHADERCROSS_SHADERSTAGE_VERTEX);
    SDL_GPUShader* fs = Compile(c.dev, kFS, SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT);
    if (!vs || !fs) return false;

    SDL_GPUColorTargetDescription cd{};
    cd.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = vs;
    pi.fragment_shader = fs;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pi.target_info.num_color_targets = 1;
    pi.target_info.color_target_descriptions = &cd;
    c.pipe = SDL_CreateGPUGraphicsPipeline(c.dev, &pi);
    SDL_ReleaseGPUShader(c.dev, vs);
    SDL_ReleaseGPUShader(c.dev, fs);
    if (!c.pipe) { SDL_Log("pipeline: %s", SDL_GetError()); return false; }

    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    ti.width = TARGET; ti.height = TARGET; ti.layer_count_or_depth = 1; ti.num_levels = 1;
    c.target = SDL_CreateGPUTexture(c.dev, &ti);

    SDL_GPUBufferCreateInfo bi{};
    bi.usage = SDL_GPU_BUFFERUSAGE_INDIRECT;
    bi.size = MAX_CMDS * CMD_BYTES;
    c.indirect = SDL_CreateGPUBuffer(c.dev, &bi);
    bi.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    bi.size = 4 * sizeof(Uint16);
    c.index = SDL_CreateGPUBuffer(c.dev, &bi);

    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size = MAX_CMDS * CMD_BYTES;
    c.staging = SDL_CreateGPUTransferBuffer(c.dev, &tbi);
    if (!c.target || !c.indirect || !c.index || !c.staging) { SDL_Log("resources: %s", SDL_GetError()); return false; }

    // Индексы — один раз.
    auto* idx = static_cast<Uint16*>(SDL_MapGPUTransferBuffer(c.dev, c.staging, false));
    idx[0] = 0; idx[1] = 1; idx[2] = 2; idx[3] = 0;
    SDL_UnmapGPUTransferBuffer(c.dev, c.staging);
    SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(c.dev);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
    SDL_GPUTransferBufferLocation src{ c.staging, 0 };
    SDL_GPUBufferRegion dst{ c.index, 0, 4 * sizeof(Uint16) };
    SDL_UploadToGPUBuffer(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    SubmitAndWait(c.dev, cb);
    return true;
}

}   // namespace

int main(int, char**)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) { SDL_Log("SDL_Init: %s", SDL_GetError()); return 1; }
    SDL_ShaderCross_Init();

    Ctx c;
    if (!Setup(c)) return 1;

    const Stat base = Measure(c, 0, false);
    SDL_Log("base (render pass, no draw): median %.3f ms, min %.3f ms", base.median, base.min);
    SDL_Log("%-9s %-6s %9s %9s %11s %11s %13s %11s", "layout", "call", "N", "draws", "median ms", "min ms", "ns/cmd (med)", "encode ms");

    const Layout layouts[] = { Layout::Empty, Layout::One, Layout::Checker, Layout::Full, Layout::HalfFull };
    for (uint32_t n : COUNTS) {
        for (Layout l : layouts) {
            const uint32_t draws = Fill(c, l, n);
            for (bool split : { false, true }) {
                const Stat s = Measure(c, draws, split);
                const double net = s.median - base.median;
                SDL_Log("%-9s %-6s %9u %9u %11.3f %11.3f %13.2f %11.3f", Name(l), split ? "Nx1" : "1xN",
                    n, draws, s.median, s.min, draws ? net * 1e6 / double(draws) : 0.0, s.encode);
            }
        }
        SDL_Log("");
    }

    SDL_ShaderCross_Quit();
    SDL_Quit();
    return 0;
}
