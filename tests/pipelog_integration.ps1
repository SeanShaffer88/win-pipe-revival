param([Parameter(Mandatory=$true)][string]$Application)
$ErrorActionPreference = 'Stop'
$Application = [IO.Path]::GetFullPath($Application)
function Start-App([string[]]$Arguments) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $Application
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    return [Diagnostics.Process]::Start($info)
}
$directory = Join-Path (Split-Path $Application -Parent) ('pipelog-integration-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$log = Join-Path $directory 'worker log.jsonl'
$pipe = 'pipelog-integration-' + [guid]::NewGuid().ToString('N')
$server = $null
$clients = @()
try {
    $server = Start-App @('serve',$pipe,$log,'12')
    $ready = $server.StandardOutput.ReadLineAsync()
    if (-not $ready.Wait(3000) -or $ready.Result -notmatch '^READY ') { throw 'Collector did not start' }
    $expected = @()
    foreach ($i in 1..12) {
        $message = 'worker ' + $i + ' "quoted" \ path ' + [char]10 + 'unicode ' + [char]0x00E9 + [char]0x6D4B
        $expected += $message
        $clients += Start-App @('send',$pipe,'info',$message,'5000')
    }
    foreach ($client in $clients) {
        if (-not $client.WaitForExit(8000) -or $client.ExitCode -ne 0) {
            throw ('Log client failed: ' + $client.StandardError.ReadToEnd())
        }
        if ($client.StandardOutput.ReadToEnd() -notmatch '^RECORDED ') { throw 'Missing application acknowledgement' }
    }
    if (-not $server.WaitForExit(8000) -or $server.ExitCode -ne 0) {
        throw ('Collector failed: ' + $server.StandardError.ReadToEnd())
    }
    $records = @(Get-Content -LiteralPath $log -Encoding UTF8 | ForEach-Object { $_ | ConvertFrom-Json })
    if ($records.Count -ne 12 -or @($records.id | Select-Object -Unique).Count -ne 12) { throw 'Lost or duplicate records' }
    foreach ($message in $expected) {
        if (@($records | Where-Object message -eq $message).Count -ne 1) { throw 'Log text was corrupted' }
    }
    $missing = Start-App @('send',($pipe + '-absent'),'warn','not delivered','100')
    try {
        if (-not $missing.WaitForExit(2000) -or $missing.ExitCode -ne 3) { throw 'Absent collector failure contract' }
    } finally { if (-not $missing.HasExited) { $missing.Kill();$missing.WaitForExit() };$missing.Dispose() }
    $invalid = Start-App @('send',$pipe,'invalid','bad level')
    try {
        if (-not $invalid.WaitForExit(2000) -or $invalid.ExitCode -eq 0) { throw 'Invalid level was accepted' }
    } finally { if (-not $invalid.HasExited) { $invalid.Kill();$invalid.WaitForExit() };$invalid.Dispose() }
    Write-Output 'PASS PipeLog: 12 concurrent processes, JSON escaping/Unicode, flushed-file acknowledgements, absent collector and invalid arguments'
} finally {
    foreach ($process in (@($server) + $clients)) {
        if ($process) { if (-not $process.HasExited) { $process.Kill();$process.WaitForExit() };$process.Dispose() }
    }
}

