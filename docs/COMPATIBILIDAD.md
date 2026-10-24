# Compatibilidad

## APIs objetivo

| API | Estado | Notas |
|---|---|---|
| OpenXR + D3D11 | Implementado | Ruta principal con capa implícita |
| OpenXR + D3D12 | Parcial | Passthrough sin upscale en esta versión |
| OpenXR + Vulkan | Parcial | Passthrough sin upscale en esta versión |
| OpenVR | Implementado (hook) | Submit/WaitGetPoses/GetHiddenAreaMesh |
| Oculus (LibOVR) | Implementado (hook) | Submit/Wait/GetFovTextureSize |

## Limitaciones conocidas

- D3D12/Vulkan sin upscale NIS en esta versión.
- Riesgo de bloqueo por anticheat/antivirus en inyección remota.
- No usar RTSS activo a la vez si el pacing interno está habilitado.
- Validación de calidad/rendimiento pendiente de hardware real VR.
