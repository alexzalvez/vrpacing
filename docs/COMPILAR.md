# Compilar VRPacing

## En GitHub Actions (recomendado)

1. Sube el repositorio a GitHub.
2. El workflow `.github/workflows/build.yml` compila en `windows-latest`.
3. Descarga el ZIP de artefacto generado.

## En local (Visual Studio 2022)

Requisitos:
- Visual Studio 2022 (Desktop development with C++)
- CMake 3.22+

Comandos (PowerShell):

```powershell
cmake --preset windows-release
cmake --build --preset windows-release --config Release
ctest --preset windows-release -C Release --output-on-failure
```

## Tests del núcleo portable (Linux/macOS/Windows)

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug --output-on-failure
```
