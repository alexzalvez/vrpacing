param(
  [string]$SourceDir = $PSScriptRoot
)

$ErrorActionPreference = 'Stop'

$target = Join-Path $env:LOCALAPPDATA 'VRPacing'
New-Item -ItemType Directory -Force -Path $target | Out-Null

Copy-Item (Join-Path $SourceDir 'VRPacingLayer.dll') -Destination $target -Force
Copy-Item (Join-Path $SourceDir 'VRPacingHook.dll') -Destination $target -Force
Copy-Item (Join-Path $SourceDir 'VRPacingTray.exe') -Destination $target -Force
Copy-Item (Join-Path $SourceDir 'VRPacingLayer.json') -Destination $target -Force

$manifest = Join-Path $target 'VRPacingLayer.json'
$regPath = 'HKCU:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit'
New-Item -Path $regPath -Force | Out-Null
New-ItemProperty -Path $regPath -Name $manifest -Value 0 -PropertyType DWord -Force | Out-Null

Write-Host "Instalado en $target y capa OpenXR registrada en HKCU."
