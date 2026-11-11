param([int]$ParentId, [string]$Target)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName PresentationFramework
$releaseUrl = 'https://api.github.com/repos/Juice-loot-is-pog/Rush-Injector/releases/latest'
$currentVersion = [version]'1.5.12'
$headers = @{ 'User-Agent' = 'Rush-Injector-Updater'; Accept = 'application/vnd.github+json' }
$backup = $null
$replaced = $false
$work = $null
function Show-Result([string]$message) {
    [System.Windows.MessageBox]::Show($message, 'Rush Injector / Update') | Out-Null
}
try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $release = Invoke-RestMethod -Uri $releaseUrl -Headers $headers -TimeoutSec 20
    $latest = [version]($release.tag_name -replace '^v', '')
    if ($latest -le $currentVersion) { Show-Result "Rush Injector is up to date ($currentVersion)."; exit }
    $binary = @($release.assets | Where-Object name -eq 'Rush-Injector.exe')
    $checksum = @($release.assets | Where-Object name -eq 'Rush-Injector.exe.sha256')
    if ($binary.Count -ne 1 -or $checksum.Count -ne 1) { throw 'The release is missing its executable or checksum.' }
    foreach ($asset in @($binary[0], $checksum[0])) {
        $uri = [uri]$asset.browser_download_url
        if ($uri.Scheme -ne 'https' -or $uri.Host -ne 'github.com' -or
            !$uri.AbsolutePath.StartsWith('/Juice-loot-is-pog/Rush-Injector/releases/download/')) {
            throw 'The update download address is invalid.'
        }
    }
    if ($binary[0].size -gt 67108864) { throw 'The update exceeds the size limit.' }
    $answer = [System.Windows.MessageBox]::Show("Rush Injector $latest is available. Download, update and restart?", 'Rush Injector / Update', 'YesNo')
    if ($answer -ne 'Yes') { exit }
    $work = Join-Path ([IO.Path]::GetTempPath()) ('RushUpdate-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $work | Out-Null
    $download = Join-Path $work 'Rush-Injector.exe'
    $sumFile = Join-Path $work 'Rush-Injector.exe.sha256'
    Invoke-WebRequest -UseBasicParsing -Uri $binary[0].browser_download_url -OutFile $download -TimeoutSec 120
    Invoke-WebRequest -UseBasicParsing -Uri $checksum[0].browser_download_url -OutFile $sumFile -TimeoutSec 30
    $expected = (Get-Content -Raw -LiteralPath $sumFile).Trim()
    if ($expected -notmatch '^[a-fA-F0-9]{64}$' -or (Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash -ne $expected) {
        throw 'Update checksum verification failed.'
    }
    $stream = [IO.File]::OpenRead($download)
    try {
        $reader = New-Object IO.BinaryReader($stream)
        if ($reader.ReadUInt16() -ne 0x5a4d) { throw 'The update is not an executable.' }
        $stream.Position = 0x3c; $offset = $reader.ReadInt32()
        if ($offset -lt 64 -or $offset -gt $stream.Length - 24) { throw 'Invalid executable headers.' }
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) { throw 'The update is not a Windows x64 executable.' }
    } finally { $stream.Dispose() }
    $parent = Get-Process -Id $ParentId -ErrorAction SilentlyContinue
    if ($parent) {
        if (!$parent.CloseMainWindow()) { throw 'Close Rush Injector and try the update again.' }
        if (!$parent.WaitForExit(30000)) { throw 'Rush Injector did not close. Try again after downloads finish.' }
    }
    $backup = $Target + '.backup-' + [guid]::NewGuid().ToString('N')
    [IO.File]::Replace($download, $Target, $backup)
    $replaced = $true
    Start-Process -FilePath $Target
    Remove-Item -LiteralPath $backup -ErrorAction SilentlyContinue
} catch {
    if ($replaced -and $backup -and (Test-Path -LiteralPath $backup)) {
        try { [IO.File]::Copy($backup, $Target, $true) } catch { }
    }
    Show-Result ('Update could not complete: ' + $_.Exception.Message)
} finally {
    if ($work -and (Test-Path -LiteralPath $work)) {
        Get-ChildItem -LiteralPath $work -File | Remove-Item -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $work -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $PSCommandPath -ErrorAction SilentlyContinue
}
