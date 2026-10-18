[CmdletBinding()]
param([string]$Manifest = (Join-Path $PSScriptRoot 'XR_APILAYER_INPUT_OVERLAY_input_hook.json'))
$ErrorActionPreference = 'Stop'
if (-not [Environment]::Is64BitProcess) { throw 'Run this script from 64-bit PowerShell.' }
# GetFullPath also works after a build folder has been removed.
if (-not [IO.Path]::IsPathRooted($Manifest)) { $Manifest = Join-Path $PWD.Path $Manifest }
$manifestPath = [IO.Path]::GetFullPath($Manifest)
$key = 'HKCU:\Software\Khronos\OpenXR\1\ApiLayers\Implicit'
if (Test-Path $key) {
    Remove-ItemProperty -Path $key -Name $manifestPath -ErrorAction SilentlyContinue
}
Write-Output "Unregistered $manifestPath. Restart the OpenXR game to unload the layer."
