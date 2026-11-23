<#
.SYNOPSIS
    Registers OpenDM Native Messaging Host for Chromium browsers and Firefox (HKCU).
#>
param(
    [string]$HostPath = "",
    [string]$ExtensionId = "ekehfpkieofbbgakjbmefknbbabjpoog"
)

$ErrorActionPreference = "Stop"

if (-not $HostPath) {
    $candidates = @(
        (Join-Path $PSScriptRoot "..\opendm-native-host.exe"),
        (Join-Path $PSScriptRoot "..\build\opendm-native-host.exe"),
        (Join-Path (Get-Location) "opendm-native-host.exe")
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) {
            $HostPath = (Resolve-Path $c).Path
            break
        }
    }
}

if (-not $HostPath -or -not (Test-Path $HostPath)) {
    Write-Error "Could not locate opendm-native-host.exe. Pass -HostPath <path\to\opendm-native-host.exe>."
}

$manifestDir = Join-Path $env:LOCALAPPDATA "OpenDM\NativeMessagingHosts"
New-Item -ItemType Directory -Force -Path $manifestDir | Out-Null

$origins = @("chrome-extension://ekehfpkieofbbgakjbmefknbbabjpoog/")
if ($ExtensionId -and $ExtensionId -ne "ekehfpkieofbbgakjbmefknbbabjpoog") {
    $origins += "chrome-extension://$ExtensionId/"
}

$chromeManifestPath = Join-Path $manifestDir "com.opendm.host.chrome.json"
$firefoxManifestPath = Join-Path $manifestDir "com.opendm.host.firefox.json"

@{
    name            = "com.opendm.host"
    description     = "OpenDM Native Messaging Host"
    path            = $HostPath
    type            = "stdio"
    allowed_origins = $origins
} | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 $chromeManifestPath

@{
    name               = "com.opendm.host"
    description        = "OpenDM Native Messaging Host"
    path               = $HostPath
    type               = "stdio"
    allowed_extensions = @("extension@opendm.org")
} | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 $firefoxManifestPath

$chromiumRegKeys = @(
    "HKCU:\Software\Google\Chrome\NativeMessagingHosts\com.opendm.host",
    "HKCU:\Software\Microsoft\Edge\NativeMessagingHosts\com.opendm.host",
    "HKCU:\Software\BraveSoftware\Brave-Browser\NativeMessagingHosts\com.opendm.host",
    "HKCU:\Software\Chromium\NativeMessagingHosts\com.opendm.host",
    "HKCU:\Software\Vivaldi\NativeMessagingHosts\com.opendm.host",
    "HKCU:\Software\Opera Software\NativeMessagingHosts\com.opendm.host"
)

foreach ($key in $chromiumRegKeys) {
    New-Item -Path $key -Force | Out-Null
    Set-ItemProperty -Path $key -Name "(Default)" -Value $chromeManifestPath
}

# Register both 64-bit and 32-bit registry views for Firefox on Windows
foreach ($view in @("Registry64", "Registry32")) {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, $view)
    $sub = $base.CreateSubKey("Software\Mozilla\NativeMessagingHosts\com.opendm.host")
    $sub.SetValue("", $firefoxManifestPath, [Microsoft.Win32.RegistryValueKind]::String)
    $sub.Close()
    $base.Close()
}

Write-Host "[OK] Registered OpenDM native host: $HostPath"
Write-Host "     Chrome/Edge/Brave manifest: $chromeManifestPath"
Write-Host "     Firefox manifest:           $firefoxManifestPath"
