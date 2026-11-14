param([switch]$CreateOnly)
$ErrorActionPreference = 'Stop'
# A byte order mark on piped text would hide git's first credential field.
[Console]::InputEncoding = [Text.UTF8Encoding]::new($false)
# Read only the GitHub credential used for this repository; never print it.
$credentialLines = "protocol=https`nhost=github.com`nusername=Juice-loot-is-pog`n`n" | git credential fill
if ($LASTEXITCODE) { throw 'GitHub sign-in is required.' }
$credential = @{}
foreach ($line in $credentialLines) {
    $pair = $line -split '=', 2
    if ($pair.Length -eq 2) { $credential[$pair[0]] = $pair[1] }
}
$headers = @{ Authorization = 'Bearer ' + $credential.password; 'User-Agent' = 'Rush-Release-Publisher'; Accept = 'application/vnd.github+json' }
$api = 'https://api.github.com/repos/Juice-loot-is-pog/Rush-Injector'
try { $repo = Invoke-RestMethod -Uri $api -Headers $headers } catch {
    if ([int]$_.Exception.Response.StatusCode -ne 404) { throw }
    $body = @{ name = 'Rush-Injector'; description = 'Rush Injector for Windows x64: official releases and corresponding source'; private = $false } | ConvertTo-Json
    $repo = Invoke-RestMethod -Method Post -Uri 'https://api.github.com/user/repos' -Headers $headers -Body $body -ContentType 'application/json'
}
Write-Output ('Repository: ' + $repo.html_url)
if ($CreateOnly) { exit }
$root = Split-Path $PSScriptRoot
$versionMatch = [regex]::Match((Get-Content -Raw (Join-Path $root 'CMakeLists.txt')), 'project\(mcbeDllInjector VERSION ([0-9]+\.[0-9]+\.[0-9]+)\)')
if (!$versionMatch.Success) { throw 'Could not determine injector version.' }
$version = $versionMatch.Groups[1].Value
$updateScript = Get-Content -Raw (Join-Path $root 'src\update_injector.ps1')
if (!$updateScript.Contains("[version]'$version'")) { throw 'Updater and project versions must match before publishing.' }
$exe = Join-Path $root 'out\build\Release\Rush Injector.exe'
$checksum = Join-Path $root 'out\build\Release\Rush-Injector.exe.sha256'
[IO.File]::WriteAllText($checksum, (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant())
$source = Join-Path $root 'out\Rush-Injector-source.zip'
$index = Join-Path $root ('out\release-index-' + [guid]::NewGuid().ToString('N'))
$previousIndex = $env:GIT_INDEX_FILE
try {
    $env:GIT_INDEX_FILE = $index
    git -C $root add -- .
    if ($LASTEXITCODE) { throw 'Could not prepare source snapshot.' }
    $tree = git -C $root write-tree
    if ($LASTEXITCODE) { throw 'Could not create source tree.' }
    git -C $root archive --format=zip -o $source $tree
    if ($LASTEXITCODE) { throw 'Could not archive release source.' }
} finally {
    $env:GIT_INDEX_FILE = $previousIndex
    Remove-Item -LiteralPath $index -ErrorAction SilentlyContinue
}
$body = @{ tag_name = "v$version"; name = "Rush Injector $version"; draft = $true; body = 'Renames the Auto Inject button to Inject Rush, including its status messages and tooltips. Includes executable, SHA-256 checksum and corresponding GPL source.' } | ConvertTo-Json
$release = Invoke-RestMethod -Method Post -Uri "$api/releases" -Headers $headers -Body $body -ContentType 'application/json'
$upload = $release.upload_url -replace '\{.*$', ''
foreach ($asset in @(@{Path=$exe;Name='Rush-Injector.exe'}, @{Path=$checksum;Name='Rush-Injector.exe.sha256'}, @{Path=$source;Name='Rush-Injector-source.zip'})) {
    Invoke-RestMethod -Method Post -Uri ($upload + '?name=' + $asset.Name) -Headers $headers -ContentType 'application/octet-stream' -InFile $asset.Path | Out-Null
}
$release = Invoke-RestMethod -Method Patch -Uri "$api/releases/$($release.id)" -Headers $headers -Body '{"draft":false}' -ContentType 'application/json'
Write-Output ('Published: ' + $release.html_url)
