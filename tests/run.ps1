param(
    [string]$Executable = (Join-Path $PSScriptRoot '../receiver_lifetime.exe'),
    [int]$Repeat = 1, [string]$ResultsPath = ''
)
$ErrorActionPreference = 'Stop'
if ($Repeat -lt 1) { throw 'Repeat must be at least 1.' }
$cases = @('default','idle','move','connected','move_construct','delivery','multi_sender','processes',
    'restart','callback','self_stop','limits','stalled','connect_abort','handshake_timeout','busy_timeout','sender_timeout','constructor','unicode',
    'event_failure','pipe_failure','thread_failure','state_failure','buffer_failure')
$results = @()
foreach ($iteration in 1..$Repeat) {
    foreach ($caseName in $cases) {
        $startInfo = New-Object System.Diagnostics.ProcessStartInfo
        $startInfo.FileName = [IO.Path]::GetFullPath($Executable)
        $startInfo.Arguments = $caseName
        $startInfo.UseShellExecute = $false
        $startInfo.CreateNoWindow = $true
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $process = [Diagnostics.Process]::Start($startInfo)
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $timer = [Diagnostics.Stopwatch]::StartNew()
        $timedOut = -not $process.WaitForExit(10000)
        if ($timedOut) { $process.Kill(); $process.WaitForExit() }
        $timer.Stop()
        $textOut = $stdout.GetAwaiter().GetResult()
        $textErr = $stderr.GetAwaiter().GetResult()
        $passed = -not $timedOut -and $process.ExitCode -eq 0
        $results += [pscustomobject]@{case=$caseName;iteration=$iteration;passed=$passed;timeout=$timedOut;exitCode=$process.ExitCode;milliseconds=$timer.ElapsedMilliseconds;stdout=$textOut.Trim();stderr=$textErr.Trim()}
        if ($passed) { Write-Output "PASS $caseName [$iteration]" }
        else { Write-Output "FAIL $caseName [$iteration]: timeout=$timedOut exit=$($process.ExitCode) $textErr" }
        $process.Dispose()
    }
}
if ($ResultsPath) { $results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $ResultsPath -Encoding UTF8 }
if (@($results | Where-Object { -not $_.passed }).Count) { throw 'Regression tests failed.' }
Write-Output ("Passed " + $results.Count + " process-isolated test executions.")

