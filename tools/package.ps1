# Build and package a release zip.  The release workflow calls this, and so can
# you -- a release must be reproducible without a runner.
#
#   .\tools\package.ps1                 # dist\gakumas-mod-runtime-dev.zip
#   .\tools\package.ps1 -Version v1.0.0
#   .\tools\package.ps1 -SkipBuild      # package whatever is already built
#
# The zip mirrors the game directory, so it extracts straight into the game root:
#
#   xinput1_3.dll
#   gakumas-mod\config.json
#   README.txt
#   LICENSE
#   third-party-notices.md
#   licenses\...
#
# .dll is not in the Gitea attachment allowlist, which is the other reason the
# release artifact is a zip rather than the bare DLL.
[CmdletBinding()]
param(
    [string]$Version = $env:GKMS_VERSION,
    [switch]$SkipBuild,
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Version) { $Version = "dev" }

# The tag is the version, but the zip name should not carry the leading v twice.
$plain = $Version -replace '^v', ''
$staging = Join-Path $repo "build\package\gakumas-mod-runtime-$plain"
$dist = Join-Path $repo "dist"
$zip = Join-Path $dist "gakumas-mod-runtime-$plain.zip"
$binaries = Join-Path $repo "build\bin\x64\$Configuration"

function Find-MSBuild {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -requires Microsoft.Component.MSBuild `
            -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
        if ($found) { return $found }
    }
    $onPath = Get-Command msbuild -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    throw "MSBuild not found. Install Visual Studio 2022 or the Build Tools."
}

if (-not $SkipBuild) {
    # premake reads GKMS_VERSION at generate time, so it has to be set before
    # project generation -- not just before the compile.
    $env:GKMS_VERSION = $Version
    Write-Host "==> generate (GKMS_VERSION=$Version)"
    & (Join-Path $repo "tools\premake5.exe") "vs2022"
    if ($LASTEXITCODE -ne 0) { throw "premake failed ($LASTEXITCODE)" }

    Write-Host "==> build $Configuration x64"
    & (Find-MSBuild) (Join-Path $repo "build\gakumas_mod_runtime.sln") `
        /m /t:Rebuild "/p:Configuration=$Configuration" /p:Platform=x64 /v:minimal
    if ($LASTEXITCODE -ne 0) { throw "MSBuild failed ($LASTEXITCODE)" }

    Write-Host "==> offline tests"
    & (Join-Path $binaries "mod_presentation_tests.exe")
    if ($LASTEXITCODE -ne 0) { throw "mod_presentation_tests failed ($LASTEXITCODE)" }
    & (Join-Path $binaries "mod_runtime_catalog_tests.exe")
    if ($LASTEXITCODE -ne 0) { throw "mod_runtime_catalog_tests failed ($LASTEXITCODE)" }
}

$dll = Join-Path $binaries "xinput1_3.dll"
if (-not (Test-Path $dll)) { throw "missing $dll -- build first, or drop -SkipBuild" }

Write-Host "==> stage"
if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
New-Item -ItemType Directory -Force (Join-Path $staging "gakumas-mod") | Out-Null
New-Item -ItemType Directory -Force (Join-Path $staging "licenses") | Out-Null
Copy-Item $dll (Join-Path $staging "xinput1_3.dll")
Copy-Item (Join-Path $repo "packaging\config.json") (Join-Path $staging "gakumas-mod\config.json")
# 自建半透明用的 shader 包（Unity 6000.0.77f1 编，与游戏同版本）。缺了它，声明了
# transparentMaterials 的 mod 会被整体拒绝并在日志里点名，不会静默变回不透明。
Copy-Item (Join-Path $repo "packaging\gmi_shaders.bundle") (Join-Path $staging "gakumas-mod\gmi_shaders.bundle")
Copy-Item (Join-Path $repo "packaging\README.txt") (Join-Path $staging "README.txt")
Copy-Item (Join-Path $repo "LICENSE") (Join-Path $staging "LICENSE")
Copy-Item (Join-Path $repo "third-party-notices.md") (Join-Path $staging "third-party-notices.md")
Copy-Item (Join-Path $repo "deps\minhook\LICENSE.txt") (Join-Path $staging "licenses\MinHook-LICENSE.txt")
Copy-Item (Join-Path $repo "src\deps\UnityResolve\LICENSE") (Join-Path $staging "licenses\UnityResolve-LICENSE.txt")
Copy-Item (Join-Path $repo "src\deps\nlohmann\LICENSE.MIT") (Join-Path $staging "licenses\nlohmann-json-LICENSE.MIT")

New-Item -ItemType Directory -Force $dist | Out-Null
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $staging "*") -DestinationPath $zip

$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
$dllHash = (Get-FileHash $dll -Algorithm SHA256).Hash
Write-Host ""
Write-Host "zip     $zip"
Write-Host "size    $((Get-Item $zip).Length) bytes"
Write-Host "sha256  $hash"
Write-Host "dll     $dllHash"

# Consumed by the workflow's release notes.
if ($env:GITHUB_OUTPUT) {
    "zip=$zip"       | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
    "name=$(Split-Path -Leaf $zip)" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
    "sha256=$hash"   | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
    "dllsha256=$dllHash" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
}
