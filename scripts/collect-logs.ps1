$ErrorActionPreference = 'Stop'

$logDir = Join-Path $env:LOCALAPPDATA 'VRPacing\logs'
$outZip = Join-Path $PWD 'vrpacing-logs.zip'

if (!(Test-Path $logDir)) {
  throw "No se encontró el directorio de logs: $logDir"
}

Compress-Archive -Path (Join-Path $logDir '*') -DestinationPath $outZip -Force
Write-Host "Logs recopilados en: $outZip"
