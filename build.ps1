param(
    [string]$QtRoot = 'C:\Qt\6.11.2\mingw_64',
    [string]$ToolsRoot = 'C:\Qt\Tools',
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
$env:PATH = "$QtRoot\bin;$ToolsRoot\mingw1310_64\bin;$env:PATH"
$cmake = Join-Path $ToolsRoot 'CMake_64\bin\cmake.exe'
$buildDir = Join-Path $PSScriptRoot 'build'
& $cmake -S $PSScriptRoot -B $buildDir -G Ninja "-DCMAKE_PREFIX_PATH=$QtRoot" "-DCMAKE_MAKE_PROGRAM=$ToolsRoot/Ninja/ninja.exe" "-DCMAKE_CXX_COMPILER=$ToolsRoot/mingw1310_64/bin/g++.exe" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw 'Configure failed' }
& $cmake --build $buildDir --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
if ($Test) {
    & "$ToolsRoot\CMake_64\bin\ctest.exe" --test-dir $buildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
}
$distDir = Join-Path $PSScriptRoot 'dist'
New-Item -ItemType Directory -Path $distDir -Force | Out-Null
Copy-Item -LiteralPath "$buildDir\VideoPlayer.exe" -Destination $distDir -Force
& "$QtRoot\bin\windeployqt.exe" --release --compiler-runtime --force "$distDir\VideoPlayer.exe"
if ($LASTEXITCODE -ne 0) { throw 'Deployment failed' }
Write-Host "Player ready: $distDir\VideoPlayer.exe"
