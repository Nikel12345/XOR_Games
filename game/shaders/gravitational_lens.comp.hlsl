[[vk::combinedImageSampler]]
Texture2D<float4>   u_scene_color            : register(t0, space0);
[[vk::combinedImageSampler]]
SamplerState        u_scene_color_sampler    : register(s0, space0);
[[vk::combinedImageSampler]]
Texture2D<float4>   u_scene_emission         : register(t1, space0);
[[vk::combinedImageSampler]]
SamplerState        u_scene_emission_sampler : register(s1, space0);
[[vk::combinedImageSampler]]
TextureCube<float4> u_environment            : register(t2, space0);
[[vk::combinedImageSampler]]
SamplerState        u_environment_sampler    : register(s2, space0);

struct CameraData { float4x4 view; float4x4 proj; };
StructuredBuffer<CameraData> Camera : register(t3, space0);

struct GravitationalLens
{
    float3 world_center;
    float  schwarzschild_radius;
    float  inner_radius;
    uint   padding0;
    uint   padding1;
    uint   padding2;
};
StructuredBuffer<GravitationalLens> Lenses : register(t4, space0);

[[vk::image_format("rgba16f")]]
RWTexture2D<float4> u_lensed_color    : register(u0, space1);
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> u_lensed_emission : register(u1, space1);

cbuffer GravitationalLensBlock : register(b0, space2)
{
    uint  lens_count;
    uint3 lens_count_padding;
};

struct ViewLens
{
    float3 axis;
    float  distance;
    float  schwarzschild_radius;
    float  inner_radius;
};

ViewLens LensInView(uint index)
{
    const float3 center = mul(Camera[0].view, float4(Lenses[index].world_center, 1.0)).xyz;
    ViewLens lens;
    lens.distance             = max(length(center), 1e-4);
    lens.axis                 = center / lens.distance;
    lens.schwarzschild_radius = Lenses[index].schwarzschild_radius;
    lens.inner_radius         = Lenses[index].inner_radius;
    return lens;
}

float3 PixelRay(float2 uv)
{
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return normalize(float3(ndc.x / Camera[0].proj[0][0], ndc.y / Camera[0].proj[1][1], -1.0));
}

bool ProjectToScreen(float3 view_ray, out float2 uv)
{
    uv = float2(0.0, 0.0);
    if (view_ray.z >= 0.0) return false;
    const float4 clip = mul(Camera[0].proj, float4(view_ray, 1.0));
    const float2 ndc  = clip.xy / clip.w;
    uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    return all(uv >= 0.0) && all(uv <= 1.0);
}

bool LensBendsPixel(ViewLens lens, float3 pixel_ray)
{
    return lens.distance > lens.inner_radius && dot(pixel_ray, lens.axis) > 0.0;
}

float ImpactParameter(ViewLens lens, float3 pixel_ray)
{
    return length(cross(pixel_ray, lens.axis)) * lens.distance;
}

float3 Deflection(ViewLens lens, float3 pixel_ray)
{
    const float  cos_angle         = dot(pixel_ray, lens.axis);
    const float3 tangent_offset    = pixel_ray / cos_angle - lens.axis;
    const float  tangent_offset_sq = max(dot(tangent_offset, tangent_offset), 1e-12);
    const float  einstein_angle_sq = 2.0 * lens.schwarzschild_radius / lens.distance;
    return -tangent_offset * (einstein_angle_sq / tangent_offset_sq) * cos_angle;
}

float3 EnvironmentColor(float3 view_ray)
{
    const float3 world_ray = mul(view_ray, (float3x3)Camera[0].view);
    return u_environment.SampleLevel(u_environment_sampler, world_ray, 0).rgb;
}

void WriteLensed(uint2 pixel, float3 color, float3 emission)
{
    u_lensed_color[pixel]    = float4(color, 1.0);
    u_lensed_emission[pixel] = float4(emission, 1.0);
}

void WriteScene(uint2 pixel, float2 uv)
{
    WriteLensed(pixel,
        u_scene_color.SampleLevel(u_scene_color_sampler, uv, 0).rgb,
        u_scene_emission.SampleLevel(u_scene_emission_sampler, uv, 0).rgb);
}

void WriteEnvironment(uint2 pixel, float3 view_ray)
{
    WriteLensed(pixel, EnvironmentColor(view_ray), float3(0.0, 0.0, 0.0));
}

[numthreads(8, 8, 1)]
void main(uint3 thread_id : SV_DispatchThreadID)
{
    uint width, height;
    u_lensed_color.GetDimensions(width, height);
    if (thread_id.x >= width || thread_id.y >= height) return;

    const uint2  pixel_index = thread_id.xy;
    const float2 pixel_uv    = (float2(pixel_index) + 0.5) / float2(width, height);
    const float3 pixel_ray   = PixelRay(pixel_uv);

    float3 source_ray = pixel_ray;
    bool   bent       = false;
    for (uint index = 0; index < lens_count; ++index) {
        const ViewLens lens = LensInView(index);
        if (!LensBendsPixel(lens, pixel_ray)) continue;
        if (ImpactParameter(lens, pixel_ray) < lens.inner_radius) {
            WriteScene(pixel_index, pixel_uv);
            return;
        }
        source_ray += Deflection(lens, pixel_ray);
        bent = true;
    }
    if (!bent) {
        WriteScene(pixel_index, pixel_uv);
        return;
    }

    source_ray = normalize(source_ray);
    float2 source_uv;
    if (!ProjectToScreen(source_ray, source_uv)) {
        WriteEnvironment(pixel_index, source_ray);
        return;
    }
    WriteScene(pixel_index, source_uv);
}
