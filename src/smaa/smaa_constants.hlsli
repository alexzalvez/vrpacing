// smaa_constants.hlsli
// Shared constant buffer for all 3 SMAA 1x Ultra compute passes (16-byte aligned).

cbuffer SmaaConstants : register(b0) {
    float4 g_rtMetrics;        // xy = (1/w, 1/h), zw = (w, h) of active viewport
    float  g_threshold;        // Edge detection threshold (0.05 = Official SMAA Ultra)
    int    g_maxSearchSteps;   // Maximum orthogonal search steps in pixels (64 = Official SMAA Ultra 32x2)
    float  g_fovealRadius;     // Foveal radius (0.60 = 60% central cone matching NIS 2.0x)
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