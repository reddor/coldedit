param([string]$Sdk = (Join-Path $PSScriptRoot 'third_party\emsdk'), [switch]$Package)
$ErrorActionPreference = 'Stop'
$rootPath = $PSScriptRoot
New-Item -ItemType Directory -Force -Path (Join-Path $rootPath 'build-web\tmp') | Out-Null
$env:EMCC_TEMP_DIR = Join-Path $rootPath 'build-web\tmp'
if (-not (Test-Path -LiteralPath (Join-Path $Sdk 'emsdk_env.ps1'))) { throw 'Install and activate Emscripten 4.0.23 first. See WEB.md.' }
$env:EMSDK_QUIET = '1'
. (Join-Path $Sdk 'emsdk_env.ps1') | Out-Null
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswherePath) {
        $installPath = & $vswherePath -latest -products '*' -property installationPath
        $cmakeRoot = Join-Path $installPath 'Common7\IDE\CommonExtensions\Microsoft\CMake'
        $env:PATH = "$(Join-Path $cmakeRoot 'CMake\bin');$(Join-Path $cmakeRoot 'Ninja');$env:PATH"
    }
}
Push-Location $rootPath
try {
    emcmake cmake -S . -B build-web -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
    if ($LASTEXITCODE -ne 0) { throw 'Web configure failed.' }
    cmake --build build-web --parallel 4
    if ($LASTEXITCODE -ne 0) { throw 'Web build failed.' }
    if ($Package) {
        cmake --install build-web --prefix dist/Web
        if ($LASTEXITCODE -ne 0) { throw 'Web packaging failed.' }
        Copy-Item -LiteralPath web\serve.mjs -Destination dist\Web\serve.mjs -Force
        Copy-Item -LiteralPath WEB.md -Destination dist\Web\WEB.md -Force
        Compress-Archive -Path dist\Web\* -DestinationPath dist\ColdEdit-web.zip -Force
    }
} finally { Pop-Location }
