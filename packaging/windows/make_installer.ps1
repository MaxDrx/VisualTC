# Builds the Windows installer from a compiled build tree (PowerShell 7):
#
#   pwsh packaging/windows/make_installer.ps1 -BuildDir build/windows-msvc-release [-OutDir dist]
#
# Produces OutDir/VisualTC-Setup-x64.exe (fixed name: the download page links
# to it). The installer needs no administrator rights: it installs for the
# current user in %LOCALAPPDATA%\Programs\VisualTC, creates the Start menu and
# desktop shortcuts and opens VisualTC at the end. IT departments can install
# for all users with /ALLUSERS (silent: /VERYSILENT /ALLUSERS).
#
# Optional Authenticode signature (removes the SmartScreen warning once the
# certificate has reputation):
#   WINDOWS_CERTIFICATE_PFX       certificate (.pfx) in base64
#   WINDOWS_CERTIFICATE_PASSWORD  its password
param(
    [Parameter(Mandatory = $true)] [string] $BuildDir,
    [string] $OutDir = "dist",
    [string] $QtDir = $env:QT_ROOT_DIR,
    [string] $Configuration = "Release"
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$root = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$match = Select-String -Path (Join-Path $root "CMakeLists.txt") -Pattern '^\s+VERSION ([0-9.]+)' | Select-Object -First 1
if (-not $match) { throw "Versão não encontrada em CMakeLists.txt" }
$version = $match.Matches[0].Groups[1].Value
$bin = Join-Path $BuildDir "bin/$Configuration"
$stage = Join-Path (Get-Location) "stage"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Write-Host "=== Arquivos do programa (VisualTC $version) ==="
Copy-Item (Join-Path $bin "VisualTC.exe"), (Join-Path $bin "visualtc-worker.exe") $stage
# DLLs that vcpkg placed next to the executables (GDCM, libarchive, zstd,
# liblzma, bzip2, zlib, OpenSSL).
Get-ChildItem (Join-Path $bin "*.dll") -ErrorAction SilentlyContinue | Copy-Item -Destination $stage
# Only VisualTC.exe uses Qt (the decoder process does not).
& (Join-Path $QtDir "bin/windeployqt.exe") --release --no-opengl-sw --no-compiler-runtime (Join-Path $stage "VisualTC.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt falhou ($LASTEXITCODE)" }

# Microsoft C++ runtime next to the program (app-local deployment, allowed by
# the Visual C++ redistribution terms): VisualTC opens even on a PC that has
# never installed the "Visual C++ Redistributable".
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio com C++ não encontrado" }
$crt = Get-ChildItem (Join-Path $vs "VC/Redist/MSVC/*/x64/Microsoft.VC14*.CRT") -Directory -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1
if (-not $crt) { throw "Runtime do Visual C++ não encontrado em $vs" }
Copy-Item (Join-Path $crt.FullName "*.dll") $stage
Write-Host "Runtime C++: $($crt.FullName)"

Copy-Item (Join-Path $root "README.md"), (Join-Path $root "THIRD_PARTY_LICENSES.md") $stage

function Invoke-Sign([string[]] $Files) {
    if (-not $env:WINDOWS_CERTIFICATE_PFX) { return }
    $temp = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
    $pfx = Join-Path $temp "visualtc-signing.pfx"
    [IO.File]::WriteAllBytes($pfx, [Convert]::FromBase64String($env:WINDOWS_CERTIFICATE_PFX))
    try {
        $signtool = Get-ChildItem (Join-Path ${env:ProgramFiles(x86)} "Windows Kits/10/bin/*/x64/signtool.exe") |
            Sort-Object FullName -Descending | Select-Object -First 1
        if (-not $signtool) { throw "signtool.exe não encontrado" }
        & $signtool.FullName sign /fd sha256 /tr http://timestamp.digicert.com /td sha256 `
            /f $pfx /p $env:WINDOWS_CERTIFICATE_PASSWORD @Files
        if ($LASTEXITCODE -ne 0) { throw "Falha na assinatura ($LASTEXITCODE)" }
    } finally {
        Remove-Item -Force $pfx -ErrorAction SilentlyContinue
    }
}
Invoke-Sign @((Join-Path $stage "VisualTC.exe"), (Join-Path $stage "visualtc-worker.exe"))

Write-Host "=== Instalador ==="
$iscc = Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6/ISCC.exe"
if (-not (Test-Path $iscc)) {
    choco install innosetup --no-progress -y | Out-Host
    if (-not (Test-Path $iscc)) { throw "Inno Setup 6 não encontrado" }
}
& $iscc "/DSourceDir=$stage" "/DOutputDir=$OutDir" "/DAppVersion=$version" (Join-Path $PSScriptRoot "visualtc.iss")
if ($LASTEXITCODE -ne 0) { throw "ISCC falhou ($LASTEXITCODE)" }
$setup = Join-Path $OutDir "VisualTC-Setup-x64.exe"
Invoke-Sign @($setup)
Get-Item $setup | Format-List Name, Length
