# FASE 0: Mediciones obligatorias en hardware real

> Ninguna conclusión definitiva debe tratarse como hecho sin estas mediciones.

## Incógnitas a medir

1. Hz reportados por:
   - OpenXR (`predictedDisplayPeriod`)
   - OpenVR (`WaitGetPoses` cadence)
   - Oculus (`ovr_WaitToBeginFrame` cadence)
   con SSW ON/OFF y distintos refresh rates.

2. Si VDXR anuncia o no `XR_KHR_visibility_mask` en cada versión.

3. Cadencia real de `WaitGetPoses` en títulos SteamVR puros y con capas intermedias.

4. Interacción con capas/mods de frame generation externo:
   - divisores 2/3/4
   - modo AFR x2

5. Coste de GPU de:
   - pase bilineal periferia
   - pase NIS central
   - coste combinado por ojo y por API.

## Criterios mínimos de aceptación

- Frametime estable en escenario fijo durante al menos 5 minutos.
- Sin doble división accidental de FPS en SSW.
- Sin artefactos visibles en bordes de la máscara.
- Sin regresión de estabilidad del runtime.
