param([Parameter(Mandatory=$true)][string]$Example)
$ErrorActionPreference = 'Stop'
function Start-Example([string]$Arguments) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = [IO.Path]::GetFullPath($Example)
    $info.Arguments = $Arguments
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    return [Diagnostics.Process]::Start($info)
}
$pipeName = 'win-pipe-example-smoke-' + [guid]::NewGuid().ToString('N')
$receiver = $null
$sender = $null
try {
    $receiver = Start-Example ("receiver " + $pipeName)
    $ready = $receiver.StandardOutput.ReadLineAsync()
    if (-not $ready.Wait(3000) -or $ready.Result -notmatch 'Listening') { throw 'Example receiver did not start' }
    $sender = Start-Example ("sender " + $pipeName)
    $sender.StandardInput.WriteLine('release-smoke')
    $sender.StandardInput.Close()
    if (-not $sender.WaitForExit(3000) -or $sender.ExitCode -ne 0) { throw 'Example sender failed' }
    $message = $receiver.StandardOutput.ReadLineAsync()
    if (-not $message.Wait(3000) -or $message.Result -ne 'Received 13 bytes: release-smoke') { throw 'Example payload was not delivered' }
    if ($sender.StandardError.ReadToEnd()) { throw 'Example sender reported an error' }
    $receiver.StandardInput.WriteLine()
    $receiver.StandardInput.Close()
    if (-not $receiver.WaitForExit(3000) -or $receiver.ExitCode -ne 0) { throw 'Example receiver failed to stop' }
    Write-Output 'PASS separate-process example smoke test'
} finally {
    foreach ($process in @($sender,$receiver)) {
        if ($process) {
            if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
            $process.Dispose()
        }
    }
}

