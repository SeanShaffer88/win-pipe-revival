param([string]$Executable = (Join-Path $PSScriptRoot '../receiver_lifetime.exe'))
$ErrorActionPreference = 'Stop'
$failed = $false
foreach ($caseName in @('default','idle','move','connected','move_construct','delivery')) {
    $process = Start-Process -FilePath $Executable -ArgumentList $caseName -WindowStyle Hidden -PassThru
    $finished = $process.WaitForExit(10000)
    if (-not $finished) {
        $process.Kill()
        $process.WaitForExit()
        Write-Output "FAIL $caseName (timeout)"
        $failed = $true
    } elseif ($process.ExitCode -ne 0) {
        Write-Output "FAIL $caseName (exit $($process.ExitCode))"
        $failed = $true
    } else {
        Write-Output "PASS $caseName"
    }
    $process.Dispose()
}
if ($failed) { exit 1 }
