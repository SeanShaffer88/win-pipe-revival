param(
    [string]$Repository = 'SeanShaffer88/win-pipe-revival',
    [Parameter(Mandatory=$true)][string]$Artifacts
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$Artifacts = [IO.Path]::GetFullPath($Artifacts)
$env:GIT_TERMINAL_PROMPT = '0'
$env:GCM_INTERACTIVE = 'Never'
$values = @{}
$headers = @{}
Push-Location $root
try {
    if (@(git status --porcelain).Count) { throw 'Commit all source changes before publishing.' }
    $commit = [string](git rev-parse HEAD)
    $source = [IO.File]::ReadAllText((Join-Path $root 'win-pipe.h'))
    if ($source -notmatch 'version = "([^"]+)"') { throw 'Header version not found.' }
    $version = $matches[1]
    $tag = 'v' + $version
    $validationPath = Join-Path $Artifacts "win-pipe-$version-validation.json"
    $validation = Get-Content -Raw -LiteralPath $validationPath | ConvertFrom-Json
    if ($validation.commit -ne $commit -or $validation.totalPassed -lt 120) { throw 'Artifacts do not match current validated commit.' }
    $sumPath = Join-Path $Artifacts "win-pipe-$version-SHA256SUMS.txt"
    foreach ($line in Get-Content -LiteralPath $sumPath) {
        if ($line -notmatch '^([a-f0-9]{64})  (win-pipe-[A-Za-z0-9._-]+)$') { throw 'Invalid checksum line.' }
        $expected=$matches[1];$name=$matches[2]
        if ((Get-FileHash -LiteralPath (Join-Path $Artifacts $name)).Hash.ToLowerInvariant() -ne $expected) {
            throw "Asset checksum failed: $name"
        }
    }
    $lines = ("protocol=https" + [char]10 + "host=github.com" + [char]10 + [char]10) | git credential fill 2>$null
    foreach ($line in $lines) { if ($line -match '^([^=]+)=(.*)$') { $values[$matches[1]]=$matches[2] } }
    if (-not $values['password']) { throw 'Authenticate Git Credential Manager for github.com before publishing.' }
    $headers=@{Authorization=('Bearer ' + $values['password']);'User-Agent'='win-pipe-release';'X-GitHub-Api-Version'='2022-11-28';Accept='application/vnd.github+json'}
    $api = 'https://api.github.com/repos/' + $Repository
    $runs = Invoke-RestMethod -Uri ($api + '/actions/runs?head_sha=' + $commit + '&per_page=30') -Headers $headers
    $green = $runs.workflow_runs | Where-Object { $_.name -eq 'Windows regression tests' -and $_.head_sha -eq $commit -and $_.conclusion -eq 'success' } | Select-Object -First 1
    if (-not $green) { throw 'No successful exact-commit Windows CI run; publication refused.' }
    $existing = $null
    try { $existing=Invoke-RestMethod -Uri ($api + '/releases/tags/' + $tag) -Headers $headers }
    catch { if ([int]$_.Exception.Response.StatusCode -ne 404) { throw } }
    if ($existing -and -not $existing.draft) { throw 'A published release already exists; refusing to overwrite it.' }
    if ($existing) { throw 'A draft exists; inspect it before resuming to avoid duplicate assets.' }
    $body = @{
        tag_name=$tag;target_commitish=$commit;name=('win-pipe ' + $version);draft=$true;prerelease=($version -match '-')
        body=("Windows C++17 named-pipe library with bounded multi-sender IPC, deterministic resource-failure tests and the PipeLog JSONL collector." +
        [Environment]::NewLine + [Environment]::NewLine + "Breaking compatibility: upgrade both peers from the original upstream protocol. Transport receipt is not durable or exactly-once application delivery. PipeLog adds application acknowledgements after output-stream flush." +
        [Environment]::NewLine + [Environment]::NewLine + "Local validation: " + $validation.totalPassed + " passing transport test executions across five configurations, plus PipeLog and example integration in every configuration. " +
        "Exact-commit Windows CI: " + $green.html_url + [Environment]::NewLine + "Review: REVIEW.md. Scope: Windows desktop local IPC, x86/x64; no ARM64 or authenticated cross-account service claim." +
        [Environment]::NewLine + "Source commit: " + $commit + ". ISC license; upstream provenance retained.")
    } | ConvertTo-Json -Depth 5
    $release = Invoke-RestMethod -Method Post -Uri ($api + '/releases') -Headers $headers -ContentType 'application/json' -Body $body
    $names = @("win-pipe-$version-source.zip","win-pipe-$version-examples.zip","win-pipe-$version-validation.json","win-pipe-$version-SHA256SUMS.txt")
    foreach ($name in $names) {
        $file = Join-Path $Artifacts $name
        $upload = 'https://uploads.github.com/repos/' + $Repository + '/releases/' + $release.id + '/assets?name=' + [Uri]::EscapeDataString($name)
        $asset = Invoke-RestMethod -Method Post -Uri $upload -Headers $headers -ContentType 'application/octet-stream' -InFile $file
        if ($asset.state -ne 'uploaded' -or $asset.size -ne (Get-Item -LiteralPath $file).Length) { throw "Asset upload verification failed: $name" }
    }
    $release = Invoke-RestMethod -Method Patch -Uri ($api + '/releases/' + $release.id) -Headers $headers -ContentType 'application/json' -Body '{"draft":false}'
    [pscustomobject]@{Release=$release.html_url;Tag=$release.tag_name;Commit=$commit;CI=$green.html_url;Assets=$names.Count;Draft=$release.draft} | ConvertTo-Json
} finally {
    $headers.Clear();$values.Clear();$lines=$null
    Pop-Location
}

