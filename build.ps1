param(
    [string]$QtRoot,
    [string]$ToolsRoot = 'C:\Qt\Tools',
    [ValidateRange(1, 64)][int]$Jobs = 4,
    [switch]$Test,
    [switch]$Portable
)
$ErrorActionPreference = 'Stop'
$arch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }
$toolPaths = @("$ToolsRoot\CMake\bin", "$ToolsRoot\CMake_64\bin", "$ToolsRoot\Ninja")
& "$PSScriptRoot/scripts/setup-msvc.ps1" -Architecture $arch
$env:PATH = ($toolPaths -join ';') + ';' + $env:PATH
$cmake = Get-Command cmake.exe -CommandType Application | Select-Object -First 1 -ExpandProperty Source
$ninja = Get-Command ninja.exe -CommandType Application | Select-Object -First 1 -ExpandProperty Source

$requireQtLto = -not $QtRoot
if ($requireQtLto) {
    $QtRoot = Join-Path $PSScriptRoot '.qt-static'
    $qtWorkDir = Join-Path $PSScriptRoot 'build/qt-static-work'
    if (-not (Test-Path -LiteralPath "$QtRoot/lto-build.json")) {
        if (-not $env:MSYS2_BASH -and (Test-Path -LiteralPath 'C:\msys64\usr\bin\bash.exe')) {
            $env:MSYS2_BASH = 'C:\msys64\usr\bin\bash.exe'
            $env:PATH += ';C:\msys64\ucrt64\bin'
        }
        & python "$PSScriptRoot/scripts/build-qt-lto.py" --static --work $qtWorkDir `
            --install $QtRoot --toolchain msvc --arch $arch --parallel $Jobs
        if ($LASTEXITCODE -ne 0) { throw 'Static Qt and FFmpeg build failed' }
    }
}
$QtRoot = (Resolve-Path -LiteralPath $QtRoot).Path
$buildDir = Join-Path $PSScriptRoot "build/static-player-msvc-$arch"
$compiler = Get-Command cl.exe -CommandType Application | Select-Object -First 1 -ExpandProperty Source
& $cmake -S $PSScriptRoot -B $buildDir -G Ninja "-DCMAKE_PREFIX_PATH=$QtRoot" "-DFFMPEG_DIR=$QtRoot/ffmpeg" `
    "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_CXX_COMPILER=$compiler" -DCMAKE_BUILD_TYPE=Release `
    "-DBUILD_TESTING=$($Test.IsPresent)" -DVIDEO_PLAYER_STATIC_WINDOWS=ON "-DVIDEO_PLAYER_REQUIRE_QT_LTO=$requireQtLto"
if ($LASTEXITCODE -ne 0) { throw 'Configure failed' }
& $cmake --build $buildDir --target VideoPlayer --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

$exe = Join-Path $buildDir 'VideoPlayer.exe'
if ($Test) {
    & "$PSScriptRoot/scripts/test-portable.ps1" -Executable $exe
    & $cmake --build $buildDir --parallel $Jobs
    if ($LASTEXITCODE -ne 0) { throw 'Test build failed' }
    & (Join-Path (Split-Path -Parent $cmake) 'ctest.exe') --test-dir $buildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
}
$distDir = Join-Path $PSScriptRoot 'dist'
New-Item -ItemType Directory -Path $distDir -Force | Out-Null
$fileName = if ($Portable) { "vp-windows-$arch.exe" } else { 'VideoPlayer.exe' }
$outputFile = Join-Path $distDir $fileName
Copy-Item -LiteralPath $exe -Destination $outputFile -Force
Write-Host "Single-file player ready: $outputFile"
