param(
    [ValidateSet('msvc','clang')][string]$Compiler = 'msvc',
    [ValidateSet('x64','x86')][string]$Architecture = 'x64',
    [ValidateSet('Release','Debug')][string]$Configuration = 'Release',
    [string]$CompilerPath = '', [int]$Repeat = 1
)
$ErrorActionPreference = 'Stop'
$savedEnvironment = @{}
foreach ($key in @('PATH','INCLUDE','LIB','LIBPATH','VCToolsVersion')) {
    $savedEnvironment[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
}
try {
$root = Split-Path $PSScriptRoot -Parent
if ($CompilerPath) {
    $leaf = [IO.Path]::GetFileNameWithoutExtension($CompilerPath)
    $Compiler = if ($leaf -eq 'cl') { 'msvc' } else { 'clang' }
}
$buildDirectory = Join-Path $root ('.build/' + $Compiler + '-' + $Architecture + '-' + $Configuration)
New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
if ($Compiler -eq 'msvc') {
    $vswhere = Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'vswhere.exe not found. Install Visual Studio C++ Build Tools.' }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) { throw 'No installed MSVC C++ toolchain found.' }
    $vcvars = Join-Path $installation 'VC/Auxiliary/Build/vcvarsall.bat'
    $target = if ($Architecture -eq 'x86') { 'x64_x86' } else { 'x64' }
    $environmentScript = Join-Path $buildDirectory 'environment.cmd'
    $lines = @('@echo off', ('call "' + $vcvars + '" ' + $target + ' >nul'),
        'if errorlevel 1 exit /b 1', 'set PATH', 'set INCLUDE', 'set LIB', 'set VCToolsVersion')
    [IO.File]::WriteAllLines($environmentScript, $lines, [Text.Encoding]::ASCII)
    $environmentLines = & $env:ComSpec /d /c call $environmentScript
    if ($LASTEXITCODE -ne 0) { throw 'MSVC environment initialization failed.' }
    foreach ($line in $environmentLines) {
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
        }
    }
    if (-not $CompilerPath) { $CompilerPath = (Get-Command cl.exe -ErrorAction Stop).Source }
} else {
    if ($Architecture -ne 'x64') { throw 'This script currently validates LLVM-MinGW x64 only.' }
    if (-not $CompilerPath) { $CompilerPath = (Get-Command clang++.exe -ErrorAction Stop).Source }
}
$exe = Join-Path $buildDirectory 'receiver_lifetime.exe'
Push-Location $root
try {
    if ($Compiler -eq 'msvc') {
        $flags = @('/nologo','/std:c++17','/EHsc','/W4','/WX','/utf-8','/D_WIN32_WINNT=0x0A00')
        if ($Configuration -eq 'Debug') { $flags += @('/Od','/Zi','/MTd') }
        else { $flags += @('/O2','/MT') }
        & $CompilerPath @flags tests/receiver_lifetime.cpp ("/Fe:$exe") ("/Fo:$buildDirectory/receiver_lifetime.obj") ("/Fd:$buildDirectory/compile.pdb")
    } else {
        $flags = @('-std=c++17','-Wall','-Wextra','-Werror','-static','-D_WIN32_WINNT=0x0A00')
        if ($Configuration -eq 'Debug') { $flags += @('-O0','-g') }
        else { $flags += '-O2' }
        & $CompilerPath @flags tests/receiver_lifetime.cpp -o $exe
    }
    if ($LASTEXITCODE -ne 0) { throw "Compiler failed: $LASTEXITCODE" }
    $exampleExe = Join-Path $buildDirectory 'win_pipe_example.exe'
    if ($Compiler -eq 'msvc') {
        & $CompilerPath @flags example.cpp ("/Fe:$exampleExe") ("/Fo:$buildDirectory/example.obj") ("/Fd:$buildDirectory/example.pdb")
    } else {
        & $CompilerPath @flags example.cpp -o $exampleExe
    }
    if ($LASTEXITCODE -ne 0) { throw "Example compilation failed: $LASTEXITCODE" }
    & (Join-Path $PSScriptRoot 'run.ps1') -Executable $exe -Repeat $Repeat -ResultsPath (Join-Path $buildDirectory 'test-results.json')
    $compilerVersion = if ($Compiler -eq 'msvc') { (Get-Item -LiteralPath $CompilerPath).VersionInfo.FileVersion }
                       else { [string]((& $CompilerPath --version | Select-Object -First 1)) }
    [ordered]@{compiler=$Compiler;compilerPath=$CompilerPath;compilerVersion=$compilerVersion;architecture=$Architecture;configuration=$Configuration;repeat=$Repeat;headerSha256=(Get-FileHash -LiteralPath 'win-pipe.h').Hash;testSha256=(Get-FileHash -LiteralPath 'tests/receiver_lifetime.cpp').Hash;exampleSha256=(Get-FileHash -LiteralPath 'example.cpp').Hash} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $buildDirectory 'build-info.json') -Encoding UTF8
} finally { Pop-Location }
} finally {
    foreach ($key in $savedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($key, $savedEnvironment[$key], 'Process')
    }
}

