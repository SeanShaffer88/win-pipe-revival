param([string]$Destination = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $Destination) { $Destination = Join-Path $root '.build/packages' }
$Destination = [IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
Push-Location $root
try {
    if (@(git status --porcelain).Count) { throw 'Commit source changes before packaging.' }
    $commit = [string](git rev-parse HEAD)
    $header = [IO.File]::ReadAllText((Join-Path $root 'win-pipe.h'))
    if ($header -notmatch 'version = "([^"]+)"') { throw 'Header version not found.' }
    $version = $matches[1]
    $headerHash = (Get-FileHash -LiteralPath 'win-pipe.h').Hash
    $testHash = (Get-FileHash -LiteralPath 'tests/receiver_lifetime.cpp').Hash
    $exampleHash = (Get-FileHash -LiteralPath 'example.cpp').Hash
    $applicationHash = (Get-FileHash -LiteralPath 'apps/pipelog/main.cpp').Hash
    $targets = @('msvc-x64-Release','msvc-x86-Release','msvc-x64-Debug','msvc-x86-Debug','clang-x64-Release')
    $evidence = @()
    foreach ($target in $targets) {
        $directory = Join-Path $root ('.build/' + $target)
        $info = Get-Content -Raw -LiteralPath (Join-Path $directory 'build-info.json') | ConvertFrom-Json
        $results = @(Get-Content -Raw -LiteralPath (Join-Path $directory 'test-results.json') | ConvertFrom-Json)
        if ($info.headerSha256 -ne $headerHash -or $info.testSha256 -ne $testHash -or $info.exampleSha256 -ne $exampleHash -or $info.applicationSha256 -ne $applicationHash) {
            throw "Stale build evidence: $target"
        }
        if ($results.Count -ne 24 * $info.repeat -or @($results | Where-Object { -not $_.passed }).Count -or -not $info.applicationIntegration -or -not $info.exampleSmoke) {
            throw "Incomplete or failing tests: $target"
        }
        $evidence += [ordered]@{
            target=$target;compilerVersion=$info.compilerVersion;passed=$results.Count
            cases=@($results.case | Select-Object -Unique);repeat=$info.repeat
        }
    }
    $totalPassed = 0
    foreach ($entry in $evidence) { $totalPassed += [int]$entry['passed'] }
    if ($totalPassed -lt 120) { throw 'Unexpected validation total.' }
    $sourceName = "win-pipe-$version-source.zip"
    $exampleName = "win-pipe-$version-examples.zip"
    $validationName = "win-pipe-$version-validation.json"
    & git archive --format=zip ("--output=" + (Join-Path $Destination $sourceName)) HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Source archive failed.' }
    # Git normalizes line endings when committing. Preserve the exact tested
    # bytes for compiled sources, after checking normalized commit equivalence.
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $sourceArchive = [IO.Compression.ZipFile]::Open((Join-Path $Destination $sourceName), [IO.Compression.ZipArchiveMode]::Update)
    try {
        foreach ($file in @('win-pipe.h','tests/receiver_lifetime.cpp','example.cpp','apps/pipelog/main.cpp')) {
            $entry = $sourceArchive.GetEntry($file)
            $reader = New-Object IO.StreamReader($entry.Open())
            try { $committed = $reader.ReadToEnd() } finally { $reader.Dispose() }
            $tested = [IO.File]::ReadAllText((Join-Path $root $file))
            if ($committed.Replace([string][char]13, '') -ne $tested.Replace([string][char]13, '')) {
                throw "Working source differs from commit: $file"
            }
            $entry.Delete()
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($sourceArchive, (Join-Path $root $file), $file)
        }
    } finally { $sourceArchive.Dispose() }
    $stage = Join-Path $root ('.build/package-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    foreach ($arch in @('x64','x86')) {
        $archDir = Join-Path $stage $arch
        New-Item -ItemType Directory -Path $archDir -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $root ('.build/msvc-' + $arch + '-Release/win_pipe_example.exe')) -Destination $archDir
        Copy-Item -LiteralPath (Join-Path $root ('.build/msvc-' + $arch + '-Release/pipelog.exe')) -Destination $archDir
    }
    foreach ($file in @('LICENSE','README.md','CHANGELOG.md','RELEASE_CHECKLIST.md','REVIEW.md')) {
        Copy-Item -LiteralPath (Join-Path $root $file) -Destination $stage
    }
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath (Join-Path $Destination $exampleName) -Force
    [ordered]@{
        version=$version;status='stable release build';commit=$commit
        headerSha256=$headerHash;testSha256=$testHash;exampleSha256=$exampleHash
        applicationSha256=$applicationHash;applicationIntegration='PipeLog integration passed in all five configurations'
        targets=$evidence;totalPassed=$totalPassed
        note='Remote CI and publication are recorded on the GitHub release page. Integration covers the included PipeLog application; no claim of arbitrary production workloads.'
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $Destination $validationName) -Encoding UTF8
    $hashLines = foreach ($name in @($sourceName,$exampleName,$validationName)) {
        ((Get-FileHash -LiteralPath (Join-Path $Destination $name)).Hash.ToLowerInvariant() + '  ' + $name)
    }
    $hashLines | Set-Content -LiteralPath (Join-Path $Destination "win-pipe-$version-SHA256SUMS.txt") -Encoding ASCII
    Write-Output ("Packaged " + $version + " at commit " + $commit)
    Write-Output ("Validated " + $totalPassed + " test executions.")
} finally { Pop-Location }

