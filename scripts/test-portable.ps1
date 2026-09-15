param([Parameter(Mandatory)][string]$Executable)
$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Executable).Path
$testDir = Join-Path ([IO.Path]::GetTempPath()) ('video-player-portable-' + [guid]::NewGuid())
New-Item -ItemType Directory $testDir | Out-Null
try {
    Copy-Item -LiteralPath $exe -Destination (Join-Path $testDir 'Video Player.exe')
    $clip = Join-Path $testDir 'test clip.mp4'
    & ffmpeg -v error -f lavfi -i 'testsrc2=size=160x90:rate=10' -t 1 -c:v mpeg4 -y $clip
    if ($LASTEXITCODE -ne 0) { throw 'Fixture generation failed' }
    $saved = @{}
    foreach ($name in @('PATH', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QT_QPA_PLATFORM', 'QML2_IMPORT_PATH')) {
        $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $null, 'Process')
    }
    try {
        $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
        # Two simultaneous first launches, then a cached launch. A relative,
        # spaced filename also checks argument forwarding and working directory.
        foreach ($count in @(2, 1)) {
            $processes = @(1..$count | ForEach-Object {
                Start-Process -FilePath (Join-Path $testDir 'Video Player.exe') -WorkingDirectory $testDir `
                    -ArgumentList '--runtime-check', '"test clip.mp4"' -WindowStyle Hidden -PassThru
            })
            foreach ($process in $processes) {
                if (-not $process.WaitForExit(60000)) { $process.Kill(); throw 'Portable playback check timed out' }
                if ($process.ExitCode -ne 0) { throw "Portable playback check failed: $($process.ExitCode)" }
            }
        }
    } finally {
        foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    }
    Write-Host 'Portable playback passed with only Windows system directories on PATH.'
} finally {
    # Only remove the exact uniquely created test directory inside the temp directory.
    $resolved = [IO.Path]::GetFullPath($testDir)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid test cleanup path' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
