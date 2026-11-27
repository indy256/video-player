param([Parameter(Mandatory)][string]$Executable)
$ErrorActionPreference = 'Stop'
$exe = (Resolve-Path -LiteralPath $Executable).Path
$testDir = Join-Path ([IO.Path]::GetTempPath()) ('video-player-portable-' + [guid]::NewGuid())
$runtimeDir = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'VideoPlayer/Runtime'
function Get-RuntimeSnapshot {
    if (Test-Path -LiteralPath $runtimeDir) {
        @(
            Get-Item -LiteralPath $runtimeDir
            Get-ChildItem -LiteralPath $runtimeDir -Recurse -Force
        ) | Sort-Object FullName | ForEach-Object { '{0}|{1:o}|{2}' -f $_.FullName, $_.LastWriteTimeUtc, $_.Length }
    }
}
New-Item -ItemType Directory $testDir | Out-Null
try {
    $copiedExe = Join-Path $testDir 'Video Player.exe'
    Copy-Item -LiteralPath $exe -Destination $copiedExe
    $clip = Join-Path $testDir 'test clip.mp4'
    & ffmpeg -v error -f lavfi -i 'testsrc2=size=160x90:rate=10' -t 1 -c:v mpeg4 -y $clip
    if ($LASTEXITCODE -ne 0) { throw 'Fixture generation failed' }
    $before = @(Get-RuntimeSnapshot)
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $copiedExe
    $start.WorkingDirectory = $testDir
    $start.Arguments = '--runtime-check "test clip.mp4" "' + $copiedExe + '"'
    $start.UseShellExecute = $false
    $start.WindowStyle = 'Hidden'
    $start.EnvironmentVariables['PATH'] = "$env:SystemRoot\System32;$env:SystemRoot"
    foreach ($name in @('QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QT_QPA_PLATFORM',
            'QML_IMPORT_PATH', 'QML2_IMPORT_PATH', 'VIDEO_PLAYER_LAUNCHER')) {
        $start.EnvironmentVariables.Remove($name)
    }
    # Run twice concurrently, then once more to check that the isolated copy
    # handles simultaneous launches and works after both processes exit.
    foreach ($count in @(2, 1)) {
        $processes = @()
        try {
            for ($i = 0; $i -lt $count; $i++) {
                $processes += [Diagnostics.Process]::Start($start)
            }
            foreach ($process in $processes) {
                if (-not $process.WaitForExit(60000)) { throw 'Portable playback check timed out' }
                if ($process.ExitCode -ne 0) { throw "Portable playback check failed: $($process.ExitCode)" }
            }
        } finally {
            foreach ($process in $processes) {
                if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
                $process.Dispose()
            }
        }
    }
    $after = @(Get-RuntimeSnapshot)
    if (($before -join "`n") -cne ($after -join "`n") -or
        @(Get-ChildItem -LiteralPath $testDir -Force).Count -ne 2) {
        throw 'Portable playback changed the Runtime cache or wrote unexpected files beside the executable'
    }
    Write-Host 'Single-executable playback passed without runtime extraction.'
} finally {
    # Only remove the exact uniquely created test directory inside the temp directory.
    $resolved = [IO.Path]::GetFullPath($testDir)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid test cleanup path' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
