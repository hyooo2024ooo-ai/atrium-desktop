<#
.SYNOPSIS
  Builds Atrium and assembles a portable folder: dist/Atrium/.

.DESCRIPTION
  native (CMake, MSVC) -> settings (dotnet publish, self-contained WinUI 3) -> dist.
  Layout:
    dist/Atrium/Atrium.exe
    dist/Atrium/Settings/Fonts/ (MiSans, D40; the core reads it from there)
    dist/Atrium/Settings/Atrium.Settings.exe (+ runtime)
  -Zip also writes dist/Atrium-<version>-win-x64.zip.
#>
param(
    [string]$Configuration = "Release",
    [switch]$Zip,
    [switch]$Development
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root $(if ($Development) { "dist/Development/Atrium" } else { "dist/Atrium" })
$buildDirectory = if ($Development) { 'native/build/development' } else { 'native/build/win-x64' }
$developmentFlag = if ($Development) { 'ON' } else { 'OFF' }

Push-Location $root
try {
    cmake --preset win-x64 -S native -B $buildDirectory "-DATRIUM_DEVELOPMENT=$developmentFlag"
    if ($LASTEXITCODE) { throw "cmake configure failed" }
    cmake --build $buildDirectory --config $Configuration
    if ($LASTEXITCODE) { throw "native build failed" }

    if (Test-Path $dist) {
        # A running Atrium (or its settings window) locks files: stop before deleting anything.
        $full = (Resolve-Path $dist).Path
        $expected = [IO.Path]::GetFullPath((Join-Path $root $(if ($Development) { 'dist/Development/Atrium' } else { 'dist/Atrium' })))
        if (![string]::Equals($full, $expected, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected distribution path' }
        $busy = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($full + '\', [StringComparison]::OrdinalIgnoreCase) })
        if ($busy) { throw "dist/Atrium is in use by $(($busy | ForEach-Object { "$($_.Name) ($($_.Id))" }) -join ', '); close it first" }
        Remove-Item -Recurse -Force $dist
    }
    New-Item -ItemType Directory -Force $dist | Out-Null
    dotnet publish settings/Atrium.Settings/Atrium.Settings.csproj -c $Configuration -p:AtriumDevelopment=$($Development.IsPresent.ToString().ToLowerInvariant()) -o (Join-Path $dist "Settings")
    if ($LASTEXITCODE) { throw "settings publish failed" }

    Copy-Item (Join-Path "$buildDirectory/$Configuration" "Atrium.exe") $dist
    Copy-Item games.txt $dist
    if ($Development) { Set-Content -LiteralPath (Join-Path $dist 'development.txt') -Value 'Development distribution: does not register or change Windows startup.' }
    Copy-Item (Join-Path $PSScriptRoot "Uninstall.ps1") $dist
    Copy-Item LICENSE $dist

    if ($Zip) {
        $version = (Select-String -Path native/CMakeLists.txt -Pattern 'VERSION (\d+\.\d+\.\d+)').Matches[0].Groups[1].Value
        $zipPath = if ($Development) { Join-Path $root "dist/Development/Atrium-$version-development-win-x64.zip" }
                   else { Join-Path $root "dist/Atrium-$version-win-x64.zip" }
        if (Test-Path $zipPath) { Remove-Item $zipPath }
        Compress-Archive -Path $dist -DestinationPath $zipPath
        Write-Host "wrote $zipPath"
    }
    Write-Host "dist ready: $dist"
}
finally {
    Pop-Location
}
