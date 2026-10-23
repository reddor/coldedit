param([switch]$Test, [switch]$Run, [switch]$CoreOnly, [switch]$Smoke, [switch]$Package)
$ErrorActionPreference = 'Stop'
$rootPath = $PSScriptRoot
$preset = if ($CoreOnly) { 'core' } else { 'release' }
# Find the installed VS C++ environment without modifying machine-wide PATH.
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    if (-not (Test-Path -LiteralPath $vswherePath)) { throw 'Install Visual Studio with Desktop development with C++.' }
    $installPath = & $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installPath) { throw 'No Visual Studio C++ toolchain found.' }
    $devShell = Join-Path $installPath 'Common7\Tools\Launch-VsDevShell.ps1'
    & $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
}
Push-Location $rootPath
try {
    cmake --preset $preset
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
    cmake --build --preset $preset --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
    if ($Test) { ctest --preset $preset; if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' } }
    if ($Smoke -and -not $CoreOnly) {
        & (Join-Path $rootPath 'tests\smoke.ps1') -Executable (Join-Path $rootPath 'build\coldedit.exe')
    }
    if ($Package -and -not $CoreOnly) {
        cmake --install build --prefix dist/ColdEdit
        if ($LASTEXITCODE -ne 0) { throw 'Packaging failed.' }
        Compress-Archive -Path (Join-Path $rootPath 'dist\ColdEdit') -DestinationPath (Join-Path $rootPath 'dist\ColdEdit-win64.zip') -Force
    }
    if ($Run -and -not $CoreOnly) { Start-Process -FilePath (Join-Path $rootPath 'build\coldedit.exe') }
} finally { Pop-Location }
