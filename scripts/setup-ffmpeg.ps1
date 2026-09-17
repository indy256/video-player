$ErrorActionPreference = 'Stop'

# This static x64 CLI only generates test clips; Windows ARM64 runs it under emulation.
# The application's FFmpeg libraries come from the native Qt kit.
$destination = Join-Path $env:RUNNER_TEMP 'ffmpeg-fixtures'
New-Item -ItemType Directory -Force $destination | Out-Null
$archive = Join-Path $destination 'ffmpeg.zip'
$url = 'https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip'
& curl.exe --fail --location --retry 3 --silent --show-error --output $archive $url
if ($LASTEXITCODE -ne 0) { throw 'FFmpeg download failed' }
$checksum = & curl.exe --fail --location --retry 3 --silent --show-error "$url.sha256"
if ($LASTEXITCODE -ne 0) { throw 'FFmpeg checksum download failed' }
if ($checksum.Trim() -notmatch '^[0-9a-fA-F]{64}$' -or
    (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $checksum.Trim()) {
    throw 'FFmpeg archive checksum mismatch'
}
Expand-Archive -LiteralPath $archive -DestinationPath $destination -Force
$executables = @(Get-ChildItem -LiteralPath $destination -Recurse -File -Filter ffmpeg.exe)
if ($executables.Count -ne 1) { throw 'Expected exactly one FFmpeg executable' }
& $executables[0].FullName -version
if ($LASTEXITCODE -ne 0) { throw 'FFmpeg failed to run' }
$executables[0].DirectoryName | Out-File -FilePath $env:GITHUB_PATH -Encoding utf8 -Append
