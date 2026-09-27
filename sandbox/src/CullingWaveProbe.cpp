// ============================================================================
//  Зонд: culling_scatter с wave-свёрткой атомиков против поштучного атомика.
//
//  Старый scatter делает InterlockedAdd на каждую видимую запись, счётчик берётся из памяти
//  (группа, уровень) — компилятор не может свернуть одинаковые адреса волны, и отсев 1М стоит
//  ~2 мс (см. память culling-perf-atomics). Новый (sandbox/shaders_code/culling_scatter_wave; в движок не взят — выигрыша нет)
//  делает один атомик на каждый различный счётчик волны («водопадный» цикл).
//
//  Данные синтетические, но с раскладкой как у движка: 1М записей подряд группами по 13000,
//  у группы 3 уровня LOD; записи — сетка 1000x1000 на плоскости, камера в центре смотрит вдоль
//  строк, так что соседние записи часто расходятся по уровню и видимости (невыгодный случай для
//  свёртки). Блоков 1 (основной проход) и 4 (каскады тени, камеры повёрнуты на 90°).
//
//  Корректность: у обоих вариантов совпадают все счётчики, а каждый кусок out_pib совпадает как
//  множество строк (порядок внутри куска задают атомики, он у вариантов разный законно).
//  Время: сабмит -> fence после SDL_WaitForGPUIdle, варианты чередуются, медиана и минимум.
//
//  CWD = src/game (шейдеры читаются относительно него). Валидация выключена: она меняет время.
// ============================================================================
#include "PCH.h"
#include <SDL3_shadercross/SDL_shadercross.h>
#include "EnginePaths.h"
#include <algorithm>
#include <vector>

namespace {

constexpr uint32_t N          = 1'000'000;
constexpr uint32_t GRID       = 1000;
constexpr uint32_t GROUP_SIZE = 13'000;
constexpr uint32_t LODS       = 3;
constexpr uint32_t MAX_BLOCKS = 4;
constexpr int      ROUNDS     = 41;
constexpr int      WARMUP     = 5;
constexpr uint32_t STATS      = 16;
constexpr int32_t  SENTINEL   = -7;

struct GroupEntry {
    uint32_t size, out_offset, gl_base, lod_count;
    float switches[3];
    uint32_t pad;
};
struct CameraData { glm::mat4 view, proj; };
struct ScatterParams {
    uint32_t range_start, range_count, num_blocks, out_base, out_cap, cnt_base, gl_count, target_height;
    float min_screen_radius_px;
    uint32_t pad[3];
};
struct ClearParams { uint32_t total; uint32_t pad[3]; };

SDL_GPUDevice* dev = nullptr;

SDL_GPUComputePipeline* MakePipeline(const char* path, const char* dump_spv = nullptr)
{
    size_t src_size = 0;
    char* src = static_cast<char*>(SDL_LoadFile(path, &src_size));
    if (!src) { SDL_Log("load failed: %s", path); return nullptr; }
    SDL_ShaderCross_HLSL_Info info{};
    info.source = src;
    info.entrypoint = "main";
    info.shader_stage = SDL_SHADERCROSS_SHADERSTAGE_COMPUTE;
    info.props = SDL_CreateProperties();
    SDL_SetStringProperty(info.props, SDL_SHADERCROSS_PROP_SPIRV_TARGET_ENV_STRING, "vulkan1.3");
    size_t size = 0;
    void* spv = SDL_ShaderCross_CompileSPIRVFromHLSL(&info, &size);
    SDL_DestroyProperties(info.props);
    SDL_free(src);
    if (!spv) { SDL_Log("compile failed %s: %s", path, SDL_GetError()); return nullptr; }
    if (dump_spv) {
        const std::string out = std::string(SDL_GetBasePath()) + dump_spv;
        SDL_SaveFile(out.c_str(), spv, size);
        SDL_Log("SPIR-V saved: %s", out.c_str());
    }

    SDL_ShaderCross_ComputePipelineMetadata* meta = SDL_ShaderCross_ReflectComputeSPIRV(static_cast<Uint8*>(spv), size, 0);
    SDL_ShaderCross_SPIRV_Info si{};
    si.bytecode = static_cast<Uint8*>(spv);
    si.bytecode_size = size;
    si.entrypoint = "main";
    si.shader_stage = SDL_SHADERCROSS_SHADERSTAGE_COMPUTE;
    SDL_GPUComputePipeline* p = meta ? SDL_ShaderCross_CompileComputePipelineFromSPIRV(dev, &si, meta, 0) : nullptr;
    if (!p) SDL_Log("pipeline failed %s: %s", path, SDL_GetError());
    SDL_free(meta);
    SDL_free(spv);
    return p;
}

SDL_GPUBuffer* MakeBuffer(Uint32 bytes, SDL_GPUBufferUsageFlags usage)
{
    SDL_GPUBufferCreateInfo ci{};
    ci.usage = usage;
    ci.size = bytes;
    return SDL_CreateGPUBuffer(dev, &ci);
}

void Upload(SDL_GPUBuffer* dst, const void* data, Uint32 bytes)
{
    SDL_GPUTransferBufferCreateInfo ti{};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    ti.size = bytes;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &ti);
    SDL_memcpy(SDL_MapGPUTransferBuffer(dev, tb, false), data, bytes);
    SDL_UnmapGPUTransferBuffer(dev, tb);
    SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
    SDL_GPUTransferBufferLocation src{ tb, 0 };
    SDL_GPUBufferRegion reg{ dst, 0, bytes };
    SDL_UploadToGPUBuffer(cp, &src, &reg, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cb);
    SDL_WaitForGPUIdle(dev);
    SDL_ReleaseGPUTransferBuffer(dev, tb);
}

template <class T>
std::vector<T> Download(SDL_GPUBuffer* src, Uint32 count)
{
    const Uint32 bytes = count * sizeof(T);
    SDL_GPUTransferBufferCreateInfo ti{};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    ti.size = bytes;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &ti);
    SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cb);
    SDL_GPUBufferRegion reg{ src, 0, bytes };
    SDL_GPUTransferBufferLocation dst{ tb, 0 };
    SDL_DownloadFromGPUBuffer(cp, &reg, &dst);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cb);
    SDL_WaitForGPUIdle(dev);
    std::vector<T> out(count);
    SDL_memcpy(out.data(), SDL_MapGPUTransferBuffer(dev, tb, false), bytes);
    SDL_UnmapGPUTransferBuffer(dev, tb);
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    return out;
}

struct Scene {
    SDL_GPUBuffer* ro[7] = {};
    SDL_GPUBuffer* out_pib = nullptr;
    SDL_GPUBuffer* counters = nullptr;
    std::vector<GroupEntry> groups;
    uint32_t out_cap = 0;
    uint32_t gl_count = 0;
};

Scene BuildScene()
{
    Scene s;
    const SDL_GPUBufferUsageFlags RO = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;

    std::vector<int32_t> pib(N);
    std::vector<uint32_t> record_group(N);
    std::vector<glm::vec4> spheres(N, glm::vec4(0.0f, 0.0f, 0.0f, 0.5f));
    std::vector<glm::mat4> transforms(N);
    for (uint32_t i = 0; i < N; ++i) {
        pib[i] = static_cast<int32_t>(i);
        record_group[i] = i / GROUP_SIZE;
        transforms[i] = glm::translate(glm::mat4(1.0f), glm::vec3(float(i % GRID), 0.0f, float(i / GRID)));
    }

    const uint32_t group_count = (N + GROUP_SIZE - 1) / GROUP_SIZE;
    uint32_t offset = 0;
    for (uint32_t g = 0; g < group_count; ++g) {
        GroupEntry e{};
        e.size = std::min(GROUP_SIZE, N - g * GROUP_SIZE);
        e.out_offset = offset;
        e.gl_base = g * LODS;
        e.lod_count = LODS;
        e.switches[0] = 20.0f;
        e.switches[1] = 5.0f;
        e.switches[2] = 0.0f;
        s.groups.push_back(e);
        offset += e.size * LODS;
    }
    s.out_cap = offset;
    s.gl_count = group_count * LODS;

    const glm::vec3 eye(500.0f, 5.0f, 500.0f);
    const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 16.0f / 9.0f, 0.1f, 2000.0f);
    std::vector<CameraData> cams(MAX_BLOCKS);
    for (uint32_t b = 0; b < MAX_BLOCKS; ++b) {
        const float a = glm::radians(90.0f * float(b));
        cams[b].view = glm::lookAt(eye, eye + glm::vec3(std::cos(a), -0.05f, std::sin(a)), glm::vec3(0, 1, 0));
        cams[b].proj = proj;
    }

    s.ro[0] = MakeBuffer(N * sizeof(int32_t), RO);                       Upload(s.ro[0], pib.data(), N * sizeof(int32_t));
    s.ro[1] = MakeBuffer(N * sizeof(uint32_t), RO);                      Upload(s.ro[1], record_group.data(), N * sizeof(uint32_t));
    s.ro[2] = MakeBuffer(group_count * sizeof(GroupEntry), RO);          Upload(s.ro[2], s.groups.data(), group_count * sizeof(GroupEntry));
    s.ro[3] = MakeBuffer(N * sizeof(glm::vec4), RO);                     Upload(s.ro[3], spheres.data(), N * sizeof(glm::vec4));
    s.ro[4] = MakeBuffer(N * sizeof(glm::mat4), RO);                     Upload(s.ro[4], transforms.data(), N * sizeof(glm::mat4));
    s.ro[5] = MakeBuffer(MAX_BLOCKS * sizeof(CameraData), RO);           Upload(s.ro[5], cams.data(), MAX_BLOCKS * sizeof(CameraData));
    s.ro[6] = MakeBuffer(sizeof(CameraData), RO);                        Upload(s.ro[6], cams.data(), sizeof(CameraData));
    s.out_pib = MakeBuffer(MAX_BLOCKS * s.out_cap * sizeof(int32_t), SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE);
    s.counters = MakeBuffer((MAX_BLOCKS * s.gl_count + STATS) * sizeof(uint32_t),
        SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE);
    return s;
}

void Clear(const Scene& s, SDL_GPUComputePipeline* clear, uint32_t num_blocks)
{
    SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUStorageBufferReadWriteBinding rw{ s.counters, false };
    SDL_GPUComputePass* cp = SDL_BeginGPUComputePass(cb, nullptr, 0, &rw, 1);
    SDL_BindGPUComputePipeline(cp, clear);
    ClearParams p{ num_blocks * s.gl_count + STATS, {} };
    SDL_PushGPUComputeUniformData(cb, 0, &p, sizeof(p));
    SDL_DispatchGPUCompute(cp, (p.total + 63) / 64, 1, 1);
    SDL_EndGPUComputePass(cp);
    SDL_SubmitGPUCommandBuffer(cb);
    SDL_WaitForGPUIdle(dev);
}

double Scatter(const Scene& s, SDL_GPUComputePipeline* scatter, uint32_t num_blocks)
{
    SDL_GPUCommandBuffer* cb = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUStorageBufferReadWriteBinding rw[2] = { { s.out_pib, false }, { s.counters, false } };
    SDL_GPUComputePass* cp = SDL_BeginGPUComputePass(cb, nullptr, 0, rw, 2);
    SDL_BindGPUComputePipeline(cp, scatter);
    SDL_BindGPUComputeStorageBuffers(cp, 0, s.ro, 7);
    ScatterParams p{};
    p.range_count = N;
    p.num_blocks = num_blocks;
    p.out_cap = s.out_cap;
    p.gl_count = s.gl_count;
    p.target_height = 1080;
    SDL_PushGPUComputeUniformData(cb, 0, &p, sizeof(p));
    SDL_DispatchGPUCompute(cp, (N + 63) / 64, 1, 1);
    SDL_EndGPUComputePass(cp);

    SDL_WaitForGPUIdle(dev);
    const Uint64 t0 = SDL_GetPerformanceCounter();
    SDL_GPUFence* f = SDL_SubmitGPUCommandBufferAndAcquireFence(cb);
    SDL_WaitForGPUFences(dev, true, &f, 1);
    const Uint64 t1 = SDL_GetPerformanceCounter();
    SDL_ReleaseGPUFence(dev, f);
    return double(t1 - t0) * 1000.0 / double(SDL_GetPerformanceFrequency());
}

struct Result { std::vector<uint32_t> counters; std::vector<int32_t> out; };

Result Snapshot(const Scene& s, uint32_t num_blocks)
{
    return { Download<uint32_t>(s.counters, num_blocks * s.gl_count), Download<int32_t>(s.out_pib, num_blocks * s.out_cap) };
}

bool Same(const Scene& s, uint32_t num_blocks, const Result& a, const Result& b, uint64_t& visible)
{
    visible = 0;
    if (a.counters != b.counters) { SDL_Log("  counters DIFFER"); return false; }
    for (uint32_t blk = 0; blk < num_blocks; ++blk) {
        for (uint32_t g = 0; g < s.groups.size(); ++g) {
            for (uint32_t L = 0; L < LODS; ++L) {
                const uint32_t n = a.counters[blk * s.gl_count + s.groups[g].gl_base + L];
                const uint32_t at = blk * s.out_cap + s.groups[g].out_offset + L * s.groups[g].size;
                std::vector<int32_t> x(a.out.begin() + at, a.out.begin() + at + n);
                std::vector<int32_t> y(b.out.begin() + at, b.out.begin() + at + n);
                std::sort(x.begin(), x.end());
                std::sort(y.begin(), y.end());
                if (x != y) { SDL_Log("  chunk DIFFERS: block %u group %u level %u", blk, g, L); return false; }
                for (uint32_t k = n; k < s.groups[g].size; ++k) {
                    if (a.out[at + k] != SENTINEL || b.out[at + k] != SENTINEL) {
                        SDL_Log("  write past count: block %u group %u level %u slot %u", blk, g, L, k);
                        return false;
                    }
                }
                visible += n;
            }
        }
    }
    return true;
}

void FillSentinel(const Scene& s)
{
    std::vector<int32_t> fill(MAX_BLOCKS * s.out_cap, SENTINEL);
    Upload(s.out_pib, fill.data(), static_cast<Uint32>(fill.size() * sizeof(int32_t)));
}

double Median(std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; }

}

int main(int, char**)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) { SDL_Log("SDL_Init: %s", SDL_GetError()); return 1; }
    SDL_ShaderCross_Init();
    SDL_GPUVulkanOptions vk{};
    vk.vulkan_api_version = (1u << 22) | (3u << 12);
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetPointerProperty(props, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER, &vk);
    dev = SDL_CreateGPUDeviceWithProperties(props);
    SDL_DestroyProperties(props);
    if (!dev) { SDL_Log("device: %s", SDL_GetError()); return 1; }

    SDL_GPUComputePipeline* clear = MakePipeline(EnginePath("shaders_code/comp/culling_clear.comp.hlsl").c_str());
    SDL_GPUComputePipeline* atomic = MakePipeline("../sandbox/shaders_code/culling_scatter_atomic.comp.hlsl");
    SDL_GPUComputePipeline* wave = MakePipeline("../sandbox/shaders_code/culling_scatter_wave.comp.hlsl", "culling_scatter_wave.spv");
    if (!clear || !atomic || !wave) return 1;

    const Scene s = BuildScene();

    for (uint32_t blocks : { 1u, 4u }) {
        FillSentinel(s); Clear(s, clear, blocks); Scatter(s, atomic, blocks);
        const Result ra = Snapshot(s, blocks);
        FillSentinel(s); Clear(s, clear, blocks); Scatter(s, wave, blocks);
        const Result rw = Snapshot(s, blocks);
        uint64_t visible = 0;
        const bool same = Same(s, blocks, ra, rw, visible);
        SDL_Log("blocks=%u: results %s, visible entries %llu of %u", blocks, same ? "IDENTICAL" : "DIFFER",
            (unsigned long long)visible, N * blocks);

        std::vector<double> ta, tw;
        for (int r = 0; r < WARMUP + ROUNDS; ++r) {
            Clear(s, clear, blocks);
            const double a = Scatter(s, atomic, blocks);
            Clear(s, clear, blocks);
            const double w = Scatter(s, wave, blocks);
            if (r >= WARMUP) { ta.push_back(a); tw.push_back(w); }
        }
        SDL_Log("blocks=%u: atomic median %.3f ms (min %.3f) | wave median %.3f ms (min %.3f)", blocks,
            Median(ta), *std::min_element(ta.begin(), ta.end()), Median(tw), *std::min_element(tw.begin(), tw.end()));
    }

    for (uint32_t blocks : { 1u, 4u }) {
        for (const char* path : { "../sandbox/shaders_code/x_atomicstats.comp.hlsl", "../sandbox/shaders_code/x_wavestats.comp.hlsl" }) {
            SDL_GPUComputePipeline* p = MakePipeline(path);
            if (!p) continue;
            Clear(s, clear, blocks);
            Scatter(s, p, blocks);
            const std::vector<uint32_t> c = Download<uint32_t>(s.counters, blocks * s.gl_count + STATS);
            const uint32_t* st = c.data() + blocks * s.gl_count;
            uint64_t visible = 0;
            for (uint32_t k = 0; k < blocks * s.gl_count; ++k) visible += c[k];
            SDL_Log("blocks=%u %-45s visible %llu, atomics %u (%.1f entries/atomic), wave size %u, max lanes per atomic %u",
                blocks, path, (unsigned long long)visible, st[0], st[0] ? double(visible) / st[0] : 0.0, st[1], st[2]);
            SDL_ReleaseGPUComputePipeline(dev, p);
        }
    }

    for (const char* path : { "../sandbox/shaders_code/x_empty.comp.hlsl", "../sandbox/shaders_code/x_readonly.comp.hlsl",
                              "../sandbox/shaders_code/x_nowrite.comp.hlsl", "../sandbox/shaders_code/x_noatomic.comp.hlsl",
                              "../sandbox/shaders_code/x_pushcounter.comp.hlsl" }) {
        SDL_GPUComputePipeline* p = MakePipeline(path);
        if (!p) continue;
        std::vector<double> t;
        for (int r = 0; r < WARMUP + ROUNDS; ++r) {
            Clear(s, clear, 1);
            const double v = Scatter(s, p, 1);
            if (r >= WARMUP) t.push_back(v);
        }
        SDL_Log("%-50s median %.3f ms (min %.3f)", path, Median(t), *std::min_element(t.begin(), t.end()));
        SDL_ReleaseGPUComputePipeline(dev, p);
    }

    SDL_WaitForGPUIdle(dev);
    SDL_DestroyGPUDevice(dev);
    SDL_ShaderCross_Quit();
    SDL_Quit();
    return 0;
}
