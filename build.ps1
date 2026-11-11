param([switch]$Test)
$ErrorActionPreference = 'Stop'
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmake) { $cmakePath = $cmake.Source } else {
    $locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path -LiteralPath $locator)) { throw 'Install Visual Studio 2022 C++ Build Tools with CMake and a Windows SDK.' }
    $install = & $locator -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $cmakePath = Join-Path $install 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
$build = Join-Path $PSScriptRoot 'out\build'
& $cmakePath -S $PSScriptRoot -B $build -G 'Visual Studio 17 2022' -A x64 -DBUILD_TESTING=ON -DwxBUILD_SAMPLES=OFF -DwxBUILD_DEMOS=OFF
if ($LASTEXITCODE) { throw 'Configuration failed.' }
& $cmakePath --build $build --config Release --target mcbeDllInjector injector_tests font_tests auto_inject_tests download_tests discord_presence_tests --parallel 6
if ($LASTEXITCODE) { throw 'Build failed.' }
if ($Test) {
    $ctest = Join-Path (Split-Path $cmakePath) 'ctest.exe'
    & $ctest --test-dir $build -C Release --output-on-failure
    if ($LASTEXITCODE) { throw 'Tests failed.' }
}
Write-Host "Built: $build\Release\Rush Injector.exe"
