#ifdef __spirv__
[[vk::binding(0, 3)]]
#endif
cbuffer SurfaceFS : register(b0, space3)
{
    float4 viewport_time; // width, height, elapsed seconds, frame index
    float4 params[4];
};

float4 ps_main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    // params[0].w selects a constant premultiplied color for deterministic readback.
    if (params[0].w >= 0.0f) return params[0];
    float wave = sin(uv.x * 5.0f + sin(uv.y * 4.0f + viewport_time.z) * 2.0f);
    float ridge = exp(-abs(uv.y - 0.5f - wave * 0.18f) * 8.0f);
    return float4(0.025f + ridge * 0.08f, 0.035f + ridge * 0.45f,
                  0.08f + ridge * 0.65f, 1.0f);
}
