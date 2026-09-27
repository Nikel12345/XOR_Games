#include "PCH.h"
#include <SDL3_shadercross/SDL_shadercross.h>
#include "EnginePaths.h"
#include <windows.h>
#include <filesystem>
#include "ShaderTypes.h"

/*
    Зонд: shadercross, собранный из исходников (external/SDL3_shadercross).

    1. dxcompiler.dll берётся из каталога exe, а не по PATH: в PATH разработчика лежит Vulkan SDK
       со своим DXC другой версии, и до вендоринга именно он молча компилировал все шейдеры.
    2. SDL_SHADERCROSS_PROP_SPIRV_TARGET_ENV_STRING (правка форка): wave-интринсики без свойства
       не собираются (SPIR-V 1.0), со свойством "vulkan1.3" собираются в SPIR-V 1.6.
    3. Все шейдеры движка под vulkan1.0 и vulkan1.3 дают одинаковую рефлексию: счётчики ресурсов
       и размер группы. Это проверяет раскладку ресурсов, которую ждёт SDL, но не картинку.
       Файлы *.vert/*.frag/*.comp собираются без дефайнов (main_pass.frag и transparent.frag —
       тела, подключаемые из surface.hlsl, сами не собираются и пропускаются); фрагментные
       варианты surface.hlsl — с дефайнами, как в DefaultShaderSet.
*/

static const std::string kInclude = EnginePath("shaders_code");

static const char* kWave =
    "RWStructuredBuffer<uint> counter : register(u0, space1);\n"
    "RWStructuredBuffer<uint> outBuf  : register(u1, space1);\n"
    "[numthreads(64, 1, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    bool visible = (id.x & 1) != 0;\n"
    "    uint n = WaveActiveCountBits(visible);\n"
    "    uint base = 0;\n"
    "    if (WaveIsFirstLane()) InterlockedAdd(counter[0], n, base);\n"
    "    base = WaveReadLaneFirst(base);\n"
    "    if (visible) outBuf[base + WavePrefixCountBits(visible)] = id.x;\n"
    "}\n";

static void* Compile(const char* source, SDL_ShaderCross_ShaderStage stage, const char* target_env, size_t& size,
    SDL_ShaderCross_HLSL_Define* defines = nullptr)
{
    SDL_ShaderCross_HLSL_Info info{};
    info.defines = defines;
    info.source = source;
    info.entrypoint = "main";
    info.include_dir = kInclude.c_str();
    info.shader_stage = stage;
    info.props = SDL_CreateProperties();
    if (target_env)
        SDL_SetStringProperty(info.props, SDL_SHADERCROSS_PROP_SPIRV_TARGET_ENV_STRING, target_env);
    void* spv = SDL_ShaderCross_CompileSPIRVFromHLSL(&info, &size);
    SDL_DestroyProperties(info.props);
    return spv;
}

static void SpirvVersion(const void* spv, Uint32& major, Uint32& minor)
{
    const Uint32 v = static_cast<const Uint32*>(spv)[1];
    major = (v >> 16) & 0xff;
    minor = (v >> 8) & 0xff;
}

static std::string Reflect(const void* spv, size_t size, SDL_ShaderCross_ShaderStage stage)
{
    char buf[256] = {};
    if (stage == SDL_SHADERCROSS_SHADERSTAGE_COMPUTE) {
        SDL_ShaderCross_ComputePipelineMetadata* m = SDL_ShaderCross_ReflectComputeSPIRV(static_cast<const Uint8*>(spv), size, 0);
        if (!m) return std::string("reflect failed: ") + SDL_GetError();
        SDL_snprintf(buf, sizeof(buf), "smp=%u rot=%u rob=%u rwt=%u rwb=%u ub=%u threads=%ux%ux%u",
            m->num_samplers, m->num_readonly_storage_textures, m->num_readonly_storage_buffers,
            m->num_readwrite_storage_textures, m->num_readwrite_storage_buffers, m->num_uniform_buffers,
            m->threadcount_x, m->threadcount_y, m->threadcount_z);
        SDL_free(m);
    } else {
        SDL_ShaderCross_GraphicsShaderMetadata* m = SDL_ShaderCross_ReflectGraphicsSPIRV(static_cast<const Uint8*>(spv), size, 0);
        if (!m) return std::string("reflect failed: ") + SDL_GetError();
        SDL_snprintf(buf, sizeof(buf), "smp=%u stex=%u sbuf=%u ub=%u in=%u out=%u",
            m->resource_info.num_samplers, m->resource_info.num_storage_textures,
            m->resource_info.num_storage_buffers, m->resource_info.num_uniform_buffers,
            m->num_inputs, m->num_outputs);
        SDL_free(m);
    }
    return buf;
}

struct Counts { int same = 0, differ = 0, skipped = 0, broke = 0; };

static void CompareOne(const std::string& path, SDL_ShaderCross_ShaderStage stage,
    SDL_ShaderCross_HLSL_Define* defines, Counts& c)
{
    int& same = c.same; int& differ = c.differ; int& skipped = c.skipped; int& broke = c.broke;
    {
        size_t src_size = 0;
        char* src = static_cast<char*>(SDL_LoadFile(path.c_str(), &src_size));
        if (!src) { SDL_Log("  [load failed] %s", path.c_str()); return; }

        size_t s10 = 0, s13 = 0;
        void* spv10 = Compile(src, stage, nullptr, s10, defines);
        const std::string err10 = spv10 ? "" : SDL_GetError();
        void* spv13 = Compile(src, stage, "vulkan1.3", s13, defines);
        const std::string err13 = spv13 ? "" : SDL_GetError();
        SDL_free(src);

        if (!spv10 && !spv13) {
            ++skipped;
            SDL_Log("  [skip, fails in both] %s", path.c_str());
        } else if (!spv10 || !spv13) {
            ++broke;
            SDL_Log("  [BROKE] %s: 1.0 %s, 1.3 %s", path.c_str(),
                spv10 ? "ok" : err10.c_str(), spv13 ? "ok" : err13.c_str());
        } else {
            const std::string r10 = Reflect(spv10, s10, stage);
            const std::string r13 = Reflect(spv13, s13, stage);
            Uint32 maj = 0, min = 0;
            SpirvVersion(spv13, maj, min);
            if (r10 == r13) {
                ++same;
            } else {
                ++differ;
                SDL_Log("  [DIFFER] %s\n    1.0: %s\n    1.3: %s", path.c_str(), r10.c_str(), r13.c_str());
            }
            if (maj != 1 || min != 6)
                SDL_Log("  [unexpected SPIR-V %u.%u] %s", maj, min, path.c_str());
        }
        SDL_free(spv10);
        SDL_free(spv13);
    }
}

static void CompareEngineShaders()
{
    Counts c;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(kInclude)) {
        const std::string path = entry.path().generic_string();
        if (path.ends_with(".vert.hlsl")) CompareOne(path, SDL_SHADERCROSS_SHADERSTAGE_VERTEX, nullptr, c);
        else if (path.ends_with(".frag.hlsl")) CompareOne(path, SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT, nullptr, c);
        else if (path.ends_with(".comp.hlsl")) CompareOne(path, SDL_SHADERCROSS_SHADERSTAGE_COMPUTE, nullptr, c);
    }

    const std::string slots = std::to_string(MAX_SLOTS);
    const std::string var_slots = std::to_string(MAX_VARIATIVE_SLOTS);
    const std::string uvl = std::to_string(MAX_UVL_BLOCKS);
    char one[] = "1";
    SDL_ShaderCross_HLSL_Define variant[] = {
        { const_cast<char*>("TEXTURE_VARIANTS"), one },
        { const_cast<char*>("MAX_VARIATIVE_SLOTS"), const_cast<char*>(var_slots.c_str()) },
        { const_cast<char*>("MAX_SLOTS"), const_cast<char*>(slots.c_str()) },
        { const_cast<char*>("MAX_UVL_BLOCKS"), const_cast<char*>(uvl.c_str()) },
        { nullptr, nullptr },
    };
    SDL_ShaderCross_HLSL_Define facing[] = {
        variant[0], variant[1], variant[2], variant[3],
        { const_cast<char*>("LIGHT_IGNORES_NORMAL"), one },
        { nullptr, nullptr },
    };
    const std::string root = kInclude;
    CompareOne(root + "/main_pass/surface.hlsl", SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT, variant, c);
    CompareOne(root + "/main_pass/surface.hlsl", SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT, facing, c);
    CompareOne(root + "/main_pass/untextured/surface.hlsl", SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT, nullptr, c);
    CompareOne("../mygame/shaders/fractal/surface.hlsl", SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT, variant, c);
    CompareOne(root + "/transparent_pass/surface.hlsl", SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT, variant, c);

    SDL_Log("engine shaders: %d same, %d differ, %d broke, %d skipped", c.same, c.differ, c.broke, c.skipped);
}

int main(int, char**)
{
    if (!SDL_ShaderCross_Init()) { SDL_Log("ShaderCross_Init failed: %s", SDL_GetError()); return 1; }

    Uint32 maj = 0, min = 0, commits = 0;
    if (SDL_ShaderCross_GetDXCVersion(&maj, &min, &commits))
        SDL_Log("DXC version: %u.%u, commit count %u", maj, min, commits);
    else
        SDL_Log("DXC version FAILED: %s", SDL_GetError());

    char path[MAX_PATH] = {};
    if (HMODULE dxc = GetModuleHandleA("dxcompiler.dll"))
        GetModuleFileNameA(dxc, path, MAX_PATH);
    SDL_Log("dxcompiler.dll loaded from: %s", path[0] ? path : "(not loaded)");

    for (const char* env : { static_cast<const char*>(nullptr), "vulkan1.3" }) {
        size_t size = 0;
        void* spv = Compile(kWave, SDL_SHADERCROSS_SHADERSTAGE_COMPUTE, env, size);
        if (spv) {
            Uint32 smaj = 0, smin = 0;
            SpirvVersion(spv, smaj, smin);
            SDL_Log("wave shader, target env %s: ok, SPIR-V %u.%u, %s", env ? env : "(default)", smaj, smin,
                Reflect(spv, size, SDL_SHADERCROSS_SHADERSTAGE_COMPUTE).c_str());
            SDL_free(spv);
        } else {
            SDL_Log("wave shader, target env %s: FAILED: %.200s", env ? env : "(default)", SDL_GetError());
        }
    }

    CompareEngineShaders();

    SDL_ShaderCross_Quit();
    return 0;
}
