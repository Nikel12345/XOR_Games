#include "PCH.h"
#include <SDL3_shadercross/SDL_shadercross.h>
#include <filesystem>
#include <fstream>


namespace fs = std::filesystem;

static void Put(const fs::path& p, const char* text)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

static bool Try(const char* label, const char* include_line, const std::string& include_dir)
{
    std::string src = std::string(include_line) + "\n"
        "float4 main(float4 p : SV_Position) : SV_Target { return float4(V, 0, 0, 1); }\n";
    SDL_ShaderCross_HLSL_Info info{};
    info.source = src.c_str();
    info.entrypoint = "main";
    info.include_dir = include_dir.c_str();
    info.shader_stage = SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT;
    info.props = SDL_CreateProperties();
    size_t size = 0;
    void* spv = SDL_ShaderCross_CompileSPIRVFromHLSL(&info, &size);
    SDL_DestroyProperties(info.props);
    SDL_Log("%-40s %s%s", label, spv ? "OK" : "FAIL ", spv ? "" : SDL_GetError());
    SDL_free(spv);
    return spv != nullptr;
}

int main(int, char**)
{
    SDL_ShaderCross_Init();
    const fs::path root = fs::temp_directory_path() / "dxc_include_probe";
    fs::remove_all(root);
    Put(root / "inc/eng.hlsli", "#define V 1\n");
    Put(root / "cwd/local.hlsli", "#define V 2\n");
    Put(root / "cwd/sub/a.hlsli", "#include \"b.hlsli\"\n");
    Put(root / "cwd/sub/b.hlsli", "#define V 3\n");
    Put(root / "inc/nest/c.hlsli", "#include \"d.hlsli\"\n");
    Put(root / "inc/nest/d.hlsli", "#define V 4\n");

    fs::current_path(root / "cwd");
    const std::string inc = (root / "inc").string();

    Try("-I:        eng.hlsli", "#include \"eng.hlsli\"", inc);
    Try("CWD:       local.hlsli", "#include \"local.hlsli\"", inc);
    Try("CWD+nest:  sub/a.hlsli -> b.hlsli", "#include \"sub/a.hlsli\"", inc);
    Try("-I+nest:   nest/c.hlsli -> d.hlsli", "#include \"nest/c.hlsli\"", inc);

    SDL_ShaderCross_Quit();
    return 0;
}
