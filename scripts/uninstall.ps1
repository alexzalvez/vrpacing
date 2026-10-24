$ErrorActionPreference = 'Stop'

$target = Join-Path $env:LOCALAPPDATA 'VRPacing'
$manifest = Join-Path $target 'VRPacingLayer.json'
$regPath = 'HKCU:\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit'

if (Test-Path $regPath) {
  Remove-ItemProperty -Path $regPath -Name $manifest -ErrorAction SilentlyContinue
}

if (Test-Path $target) {
  Remove-Item -Path $target -Recurse -Force
}

Write-Host 'VRPacing desinstalado.'
