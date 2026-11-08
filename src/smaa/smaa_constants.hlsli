// smaa_constants.hlsli
// Constant buffer compartido por los tres passes de SMAA (alineado a 16 bytes).

cbuffer SmaaConstants : register(b0) {
    float4 g_rtMetrics;        // xy = (1/width, 1/height), zw = (width, height) del viewport activo
    float  g_threshold;        // Umbral de deteccion de bordes (0.05 = Ultra)
    int    g_maxSearchSteps;   // Pasos maximos de busqueda ortogonal (32)
    float  g_fovealRadius;     // Radio foveal (0.60 = mismo cono central que NIS 2.0x)
    float  _pad0;              // Padding 16-byte
    float2 g_projectionCenter; // Centro de proyeccion UV [0..1]
    uint2  g_viewportOffset;   // Offset en pixeles (x, y) dentro de la textura fuente
};

// Devuelve 1.0 dentro del radio foveal configurado (60%), 0.0 fuera.
inline float InFovealMask(float2 uv) {
    float2 d = uv - g_projectionCenter;
    d.x *= (g_rtMetrics.z * g_rtMetrics.y); // Correccion de aspecto (width / height)
    float r = 0.5f * g_fovealRadius;
    return (dot(d, d) <= (r * r)) ? 1.0f : 0.0f;
}
