param (
    [ValidateSet('x64', 'arm64')]
    [string]$Architecture = 'x64'
)

$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$installation = & $vswhere -latest -products '*' -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $installation) {
    throw 'Visual Studio installation not found'
}

$before = @{}
Get-ChildItem Env: | ForEach-Object { $before[$_.Name] = $_.Value }
$hostArchitecture = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }
# Older Developer PowerShell launchers reject native ARM64 hosts even when the
# installed compiler supports them. Use the batch entry point directly.
$command = 'call "{0}\Common7\Tools\VsDevCmd.bat" -no_logo -arch={1} -host_arch={2} >nul && set' -f `
    $installation, $Architecture, $hostArchitecture
$environmentLines = & $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) {
    throw "Visual Studio environment setup failed with exit code $LASTEXITCODE"
}
foreach ($line in $environmentLines) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
if ($env:VSCMD_ARG_TGT_ARCH -ne $Architecture) {
    throw "Visual Studio did not select the requested architecture: $Architecture"
}
Get-Command cl.exe, link.exe, rc.exe | Select-Object Name, Source

# Make the developer environment available to subsequent workflow steps.
if ($env:GITHUB_ENV) {
    Get-ChildItem Env: | Where-Object { $before[$_.Name] -cne $_.Value } | ForEach-Object {
        $delimiter = [Guid]::NewGuid().ToString('N')
        "$($_.Name)<<$delimiter`n$($_.Value)`n$delimiter" |
            Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
    }
}
