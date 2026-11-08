// smaa_constants.hlsli
// Shared constant buffer for all 3 SMAA compute passes (16-byte aligned).

cbuffer SmaaConstants : register(b0) {
    float4 g_rtMetrics;        // xy = (1/w, 1/h), zw = (w, h) of active viewport
    float  g_threshold;        // Edge detection threshold (0.05 = High sensitivity / SMAA Ultra)
    int    g_maxSearchSteps;   // Maximum search steps (16)
    float  g_fovealRadius;     // Foveal radius (0.60 = 60% central cone)
    float  _pad0;              // 16-byte boundary padding
    float2 g_projectionCenter; // UV projection center [0..1]
    uint2  g_viewportOffset;   // Pixel offset (x, y) inside source swapchain
};

inline float InFovealMask(float2 uv) {
    float2 d = uv - g_projectionCenter;
    d.x *= (g_rtMetrics.z * g_rtMetrics.y); // Aspect ratio correction (w / h)
    float r = 0.5f * g_fovealRadius;
    return (dot(d, d) <= (r * r)) ? 1.0f : 0.0f;
}