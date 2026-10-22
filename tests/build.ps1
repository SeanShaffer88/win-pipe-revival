param([string]$CompilerPath = 'clang++')
$ErrorActionPreference = 'Stop'
Push-Location (Join-Path $PSScriptRoot '..')
try {
    $compilerName = [IO.Path]::GetFileNameWithoutExtension($CompilerPath)
    if ($compilerName -eq 'cl') {
        & $CompilerPath /nologo /std:c++17 /EHsc /W4 /O2 tests/receiver_lifetime.cpp /Fe:receiver_lifetime.exe /Fo:receiver_lifetime.obj
    } else {
        & $CompilerPath -std=c++17 -O2 -Wall -Wextra -static tests/receiver_lifetime.cpp -o receiver_lifetime.exe
    }
    if ($LASTEXITCODE -ne 0) { throw "Compiler failed with exit $LASTEXITCODE" }
    & (Join-Path $PSScriptRoot 'run.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'Regression tests failed' }
} finally { Pop-Location }
