# Registers only when explicitly run; the build/install never edits the registry.
[CmdletBinding()]
param([string]$Manifest = (Join-Path $PSScriptRoot 'XR_APILAYER_INPUT_OVERLAY_input_hook.json'))
$ErrorActionPreference = 'Stop'
if (-not [Environment]::Is64BitProcess) { throw 'Run this script from 64-bit PowerShell.' }
$manifestPath = (Resolve-Path -LiteralPath $Manifest).Path
$description = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($description.api_layer.name -ne 'XR_APILAYER_INPUT_OVERLAY_input_hook') { throw 'Unexpected layer manifest.' }
$libraryPath = Join-Path (Split-Path $manifestPath) $description.api_layer.library_path
if (-not (Test-Path -LiteralPath $libraryPath -PathType Leaf)) { throw "Layer DLL is missing: $libraryPath" }
$key = 'HKCU:\Software\Khronos\OpenXR\1\ApiLayers\Implicit'
New-Item -Path $key -Force | Out-Null
New-ItemProperty -Path $key -Name $manifestPath -Value 0 -PropertyType DWord -Force | Out-Null
Write-Output "Registered $manifestPath. Restart the OpenXR game to load the layer."
