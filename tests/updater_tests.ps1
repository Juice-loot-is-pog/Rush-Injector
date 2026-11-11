$ErrorActionPreference = 'Stop'
$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\src\update_injector.ps1')
# Substitute UI/network/process boundaries; execute real verification and replacement on fixtures.
$source = $source.Replace('Add-Type -AssemblyName PresentationFramework', '')
$source = $source.Replace("[System.Windows.MessageBox]::Show(`$message, 'Rush Injector / Update') | Out-Null", '$script:messages += $message')
$source = [regex]::Replace($source, '(?m)^    \$answer = .*$', '    $answer = ''Yes''')
$source = $source.Replace('Remove-Item -LiteralPath $PSCommandPath -ErrorAction SilentlyContinue', '')
$update = [scriptblock]::Create($source)
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('RushUpdaterTest-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
$target = Join-Path $fixture 'Injector.exe'
$payload = New-Object byte[] 128
$payload[0] = 0x4d; $payload[1] = 0x5a; $payload[0x3c] = 64
$payload[64] = 0x50; $payload[65] = 0x45; $payload[68] = 0x64; $payload[69] = 0x86
$payloadPath = Join-Path $fixture 'payload.exe'
[IO.File]::WriteAllBytes($payloadPath, $payload)
$script:digest = (Get-FileHash -LiteralPath $payloadPath).Hash
$script:valid = $false
$script:started = $false
$script:messages = @()
function Show-TestMessage($message, $title, $buttons) {
    $script:messages += $message
    if ($buttons -eq 'YesNo') { return 'Yes' }
}
function Invoke-RestMethod {
    return @{tag_name='v99.0.0'; assets=@(
        @{name='Rush-Injector.exe';size=128;browser_download_url='https://github.com/Juice-loot-is-pog/Rush-Injector/releases/download/v99.0.0/Rush-Injector.exe'},
        @{name='Rush-Injector.exe.sha256';size=64;browser_download_url='https://github.com/Juice-loot-is-pog/Rush-Injector/releases/download/v99.0.0/Rush-Injector.exe.sha256'})}
}
function Invoke-WebRequest {
    param($Uri, $OutFile, $TimeoutSec, [switch]$UseBasicParsing)
    if ($Uri.EndsWith('.sha256')) {
        [IO.File]::WriteAllText($OutFile, $(if ($script:valid) {$script:digest} else {'0' * 64}))
    } else { [IO.File]::WriteAllBytes($OutFile, $payload) }
}
function Start-Process { param($FilePath); $script:started = $true }
try {
    [IO.File]::WriteAllText($target, 'original')
    & $update -ParentId 2147483647 -Target $target
    if ((Get-Content -Raw -LiteralPath $target) -ne 'original' -or $script:started) { throw 'Invalid checksum modified or launched the target.' }
    if (!($script:messages -match 'checksum verification failed')) { throw 'Checksum error was not reported.' }
    $script:valid = $true
    & $update -ParentId 2147483647 -Target $target
    if ((Get-FileHash -LiteralPath $target).Hash -ne $script:digest -or !$script:started) { throw 'Valid update did not replace and launch the target.' }
    Write-Output 'PASS: invalid checksum preserves original; valid x64 update replaces and relaunches.'
} finally {
    Get-ChildItem -LiteralPath $fixture -File | Remove-Item -Force
    Remove-Item -LiteralPath $fixture
}
