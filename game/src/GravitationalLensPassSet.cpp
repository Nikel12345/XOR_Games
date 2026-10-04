#include "PCH.h"
#include "GravitationalLensPassSet.h"
#include "GravitationalLensDataModule.h"
#include "EngineContext.h"
#include "PassManager.h"
#include "ShaderManager.h"
#include "TextureManager.h"
#include "BufferManager.h"
#include "ParamsSpec.h"
#include "DefaultRenderPassSet.h"
#include "TexturesPresets.h"

namespace
{
    constexpr const char* LENS_PROGRAM          = "gravitational_lens";
    constexpr const char* LENSED_COLOR_ATLAS    = "lensed_scene_color";
    constexpr const char* LENSED_EMISSION_ATLAS = "lensed_scene_emission";
    constexpr const char* SCENE_COLOR_ATLAS     = "scene_hdr";
    constexpr const char* SCENE_EMISSION_ATLAS  = "scene_emission";

    struct alignas(16) LensCountPushData {
        uint32_t lens_count;
        uint32_t padding[3];
    };

    uint32_t SceneTargetDimension(uint32_t base_dimension)
    {
        const float scale = DefaultRenderPassNamespace::SceneResolutionScale();
        if (!(scale > 0.0f)) return 1;
        return safe_f_u32(std::clamp(std::floor(static_cast<float>(base_dimension) * scale), 1.0f, 16384.0f));
    }

    SDL_GPUTextureCreateInfo LensedTargetCreateInfo(uint32_t base_width, uint32_t base_height)
    {
        return TexturePresets::SceneHDR(SceneTargetDimension(base_width), SceneTargetDimension(base_height));
    }

    void CopyWholeTexture(SDL_GPUCommandBuffer* command_buffer, const TextureAtlas* source, const TextureAtlas* destination)
    {
        SDL_GPUTextureLocation source_location{};
        source_location.texture = source->texture_binding.texture;
        SDL_GPUTextureLocation destination_location{};
        destination_location.texture = destination->texture_binding.texture;

        SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(command_buffer);
        SDL_CopyGPUTextureToTexture(copy_pass, &source_location, &destination_location,
            destination->width, destination->height, 1, false);
        SDL_EndGPUCopyPass(copy_pass);
    }

    void BlitWholeTexture(SDL_GPUCommandBuffer* command_buffer, const TextureAtlas* source, const TextureAtlas* destination)
    {
        SDL_GPUBlitInfo blit_info{};
        blit_info.source.texture = source->texture_binding.texture;
        blit_info.source.w = source->width;
        blit_info.source.h = source->height;
        blit_info.destination.texture = destination->texture_binding.texture;
        blit_info.destination.w = destination->width;
        blit_info.destination.h = destination->height;
        blit_info.load_op = SDL_GPU_LOADOP_DONT_CARE;
        blit_info.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(command_buffer, &blit_info);
    }
}

void GravitationalLensPassSet::SetPasses(EngineContext* ctx, uint32_t base_width, uint32_t base_height,
                                         GravitationalLensDataModule* gravitational_lens_data)
{
    namespace RP = DefaultRenderPassNamespace;

    TextureManager* texture_manager = ctx->GetTextureManager();
    ShaderManager*  shader_manager  = ctx->GetShaderManager();
    PassManager*    pass_manager    = ctx->GetPassManager();
    BufferManager*  buffer_manager  = ctx->GetBufferManager();

    TextureAtlas* scene_color    = ctx->GetTextureAtlas(SCENE_COLOR_ATLAS);
    TextureAtlas* scene_emission = ctx->GetTextureAtlas(SCENE_EMISSION_ATLAS);
    if (!scene_color || !scene_emission) {
        SDL_Log("GravitationalLensPassSet::SetPasses: scene targets are missing - lens is not created");
        return;
    }

    TextureAtlas* lensed_color = ctx->CreateTextureAtlas(LENSED_COLOR_ATLAS, LensedTargetCreateInfo(base_width, base_height),
        DefaultSamplersNames::SIMPLE_SAMPLER, ResourceTag::CodeOwned | ResourceTag::System);
    TextureAtlas* lensed_emission = ctx->CreateTextureAtlas(LENSED_EMISSION_ATLAS, LensedTargetCreateInfo(base_width, base_height),
        DefaultSamplersNames::SIMPLE_SAMPLER, ResourceTag::CodeOwned | ResourceTag::System);
    lensed_color->tci.usage    |= SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
    lensed_emission->tci.usage |= SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    auto follow_scene_size = [texture_manager](const char* atlas_name, TextureAtlas* atlas) {
        texture_manager->CreateResizeInstruction(atlas_name,
            [atlas](TextureManager& textures, uint32_t width, uint32_t height) {
                textures.RecreateAtlasTexture(atlas, LensedTargetCreateInfo(width, height));
            });
    };
    follow_scene_size(LENSED_COLOR_ATLAS, lensed_color);
    follow_scene_size(LENSED_EMISSION_ATLAS, lensed_emission);

    ComputePassStep* lens_step = pass_manager->CreateComputePass(LENS_PASS,
        [gravitational_lens_data, buffer_manager, scene_color, scene_emission, lensed_color, lensed_emission]
        (SDL_GPUCommandBuffer* command_buffer, PassManager* passes, ComputePassStep& step, uint8_t pass_frame)
    {
        LensCountPushData* push_data = step.State<LensCountPushData>();
        if (!push_data) return;
        push_data->lens_count = gravitational_lens_data->AskLensCount(pass_frame);
        if (push_data->lens_count == 0) return;

        if (!scene_color->texture_binding.texture || !scene_emission->texture_binding.texture
            || !lensed_color->texture_binding.texture || !lensed_emission->texture_binding.texture) return;

        RP::DummyDispatchData dispatch_data{};
        passes->ComputePassStandardBody(command_buffer, &step, buffer_manager, step.state.data(), &dispatch_data, pass_frame);

        CopyWholeTexture(command_buffer, lensed_color, scene_color);
        BlitWholeTexture(command_buffer, lensed_emission, scene_emission);
    },
        PassAnchor::After(RP::AO_PASS));
    if (!lens_step) return;
    SetPassState(lens_step, LensCountPushData{});

    shader_manager->CreateComputePushInstruction<LensCountPushData>(LENS_PROGRAM,
        [](const PushConstantBinder& binder, LensCountPushData push_data) { binder.Push(push_data); });
    shader_manager->CreateDispatchInstruction<RP::DummyDispatchData>(LENS_PROGRAM,
        [lensed_color](DispatchSizeBinder& binder, RP::DummyDispatchData) {
            binder.Dispatch(lensed_color->width, lensed_color->height);
        });
}
