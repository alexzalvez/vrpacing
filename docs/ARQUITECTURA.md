# Arquitectura de VRPacing

## Subsistemas principales

1. **Núcleo portable (`vrpacing_core`)**
   - Geometría de máscara elíptica fuera-del-óvalo.
   - Matemática pura de limitador sin deriva.
   - Tracker de Hz con suavizado y diagnóstico.

2. **Ruta OpenXR (capa implícita)**
   - Intercepta `xrWaitFrame` para pacing antes del runtime.
   - Intercepta máscara de visibilidad para inyectar malla elíptica.
   - Gestiona swapchain proxy y escalado en ruta D3D11.

3. **Ruta OpenVR/Oculus (hooks)**
   - Hook de `WaitGetPoses` / `ovr_WaitToBeginFrame` para pacing.
   - Hook de `Submit` para transformar textura antes de compositor.

## Por qué se descarta FFR/VRS

- Coste alto de compatibilidad entre APIs/motores.
- Historial de inestabilidad en DX12/UE5.
- Beneficio menos consistente que NIS 2.0x en este alcance.

## Por qué sin RTSS

- Se evita doble control del pacing.
- Menor dependencia externa y menor fricción de instalación.

## Dónde espera el limitador y por qué

La espera se ejecuta **antes de la llamada al runtime** (`xrWaitFrame`, `WaitGetPoses`, `ovr_WaitToBeginFrame`) para evitar bloquear la etapa posterior de envío y mantener latencia más estable.
