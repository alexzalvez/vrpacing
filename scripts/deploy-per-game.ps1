param(
  [Parameter(Mandatory=$true)] [string]$GameDir,
  [string]$SourceDir = (Join-Path $env:LOCALAPPDATA 'VRPacing')
)

$ErrorActionPreference = 'Stop'

if (!(Test-Path $GameDir)) { throw "No existe la ruta: $GameDir" }

# Alternativa de bajo riesgo para entornos con anticheat estricto.
Copy-Item (Join-Path $SourceDir 'VRPacingHook.dll') -Destination (Join-Path $GameDir 'dxgi.dll') -Force
Copy-Item (Join-Path $SourceDir 'config.json') -Destination (Join-Path $GameDir 'vrpacing.json') -ErrorAction SilentlyContinue

Write-Host "Despliegue por juego completado en $GameDir"
