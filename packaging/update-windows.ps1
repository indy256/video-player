param([Parameter(Mandatory=$true)][string]$PlanPath)
$ErrorActionPreference = 'Stop'
$stage = [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($PlanPath))
$log = Join-Path $stage 'update.log'
$replaced = $false
function Restart-Player {
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $target
    $start.WorkingDirectory = [IO.Path]::GetDirectoryName($target)
    $start.UseShellExecute = $false
    if ($plan.video) {
        # A Windows filename cannot contain quotes. Escape trailing backslashes.
        $start.Arguments = '"' + ([regex]::Replace([string]$plan.video, '(\\+)$', '$1$1')) + '"'
    }
    $null = [Diagnostics.Process]::Start($start)
}
try {
    $plan = Get-Content -LiteralPath $PlanPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $target = [IO.Path]::GetFullPath($plan.target)
    $source = [IO.Path]::GetFullPath($plan.source)
    if ([IO.Path]::GetDirectoryName($source) -ne $stage -or
        [IO.Path]::GetDirectoryName($stage) -ne [IO.Path]::GetDirectoryName($target) -or
        !(Test-Path -LiteralPath $target -PathType Leaf) -or
        !(Test-Path -LiteralPath $source -PathType Leaf)) { throw 'Invalid update paths.' }
    $playerProcess = Get-Process -Id $plan.pid -ErrorAction Stop
    [IO.File]::WriteAllText((Join-Path $stage 'ready'), 'ready')
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while (!(Test-Path -LiteralPath (Join-Path $stage 'commit'))) {
        if ([DateTime]::UtcNow -ge $deadline) { throw 'Installation was not committed.' }
        Start-Sleep -Milliseconds 100
    }
    if (!$playerProcess.WaitForExit(120000)) { throw 'The player did not close in time.' }
    # The portable launcher's parent process exits just after the player does.
    for ($attempt = 0; $attempt -lt 40; $attempt++) {
        try {
            [IO.File]::Replace($source, $target, [System.Management.Automation.Language.NullString]::Value)
            $replaced = $true
            break
        } catch {
            if ($attempt -eq 39) { throw }
            Start-Sleep -Milliseconds 500
        }
    }
    Restart-Player
    [IO.File]::WriteAllText($log, 'Update installed.')
} catch {
    $failure = $_.Exception.Message
    [IO.File]::WriteAllText($log, $failure)
    if (Test-Path -LiteralPath (Join-Path $stage 'commit')) {
        if (!$replaced -and $playerProcess -and $playerProcess.HasExited) {
            try { Restart-Player } catch {}
        }
        Add-Type -AssemblyName System.Windows.Forms
        [Windows.Forms.MessageBox]::Show($failure + "`nDetails: " + $log, 'Video Player update failed') | Out-Null
    }
    exit 1
}
