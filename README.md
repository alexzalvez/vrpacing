# VRPacing

VRPacing es un proyecto nativo para Windows x64 enfocado en **frame pacing estable en VR** y mejora de imagen con **NIS 2.0x** en el punto de entrega al runtime.

## Qué hace

1. **Limitador de FPS de precisión** interno (sin RTSS), con objetivo:
   - `targetFps = hz * afrMultiplier / frameGenDivisor - offset`
   - por defecto: `offset = 0.1`
2. **Escalado NIS 2.0x** en D3D11:
   - región central con pase NIS
   - periferia con escalado barato
3. **Máscara de visibilidad elíptica** configurable por centro/radios.
4. **Cobertura de APIs**:
   - capa OpenXR implícita
   - hooks OpenVR y Oculus (LibOVR)

## Instalación rápida

1. Compila desde GitHub Actions o local (ver `docs/COMPILAR.md`).
2. Descomprime artefactos en una carpeta.
3. Ejecuta `scripts/install.ps1` en PowerShell.
4. Lanza `VRPacingTray.exe`.

## Uso rápido

- Ajusta `config.json` en `%LOCALAPPDATA%\VRPacing\config.json`.
- Activa/desactiva pacing, NIS y máscara.
- Para desinstalar: `scripts/uninstall.ps1`.

## Matriz de modos recomendados

- **SSW automático (VDXR)**: modo 1:1 (sin divisor adicional).
- **Frame generation externo x2/x3/x4**: usar divisores `2/3/4`.
- **Escenarios AFR**: `afrMultiplier = 2`.

## Estado del proyecto

- Núcleo portable (geometría/matemática/tracker) con tests ejecutables en Linux.
- Implementación Windows orientada a compilación en `windows-latest` por GitHub Actions.
- D3D12/Vulkan en passthrough en esta versión.

## Riesgos y advertencias

- Inyección DLL puede disparar antivirus/anticheat.
- Se ofrece alternativa de menor riesgo: `scripts/deploy-per-game.ps1`.
- Evita doble limitador (RTSS + VRPacing a la vez).

## Hoja de ruta resumida

- Completar cobertura de upscale para D3D12/Vulkan.
- Afinar máscara por ojo y validación en hardware real.
- Telemetría detallada de frametime y latencia extremo a extremo.
