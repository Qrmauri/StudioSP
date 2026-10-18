param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
$cmakePath = if ($cmakeCommand) { $cmakeCommand.Source } else { $null }
if (-not $cmakePath) {
    $vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswherePath) {
        $installation = & $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($installation) {
            $candidate = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path -LiteralPath $candidate) { $cmakePath = $candidate }
        }
    }
}
if (-not $cmakePath) { throw 'Se requiere CMake y Visual Studio Build Tools con herramientas C++.' }
$buildPath = Join-Path $projectRoot 'build'
& $cmakePath -S $projectRoot -B $buildPath
if ($LASTEXITCODE -ne 0) { throw 'La configuración falló.' }
& $cmakePath --build $buildPath --config $Configuration --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'La compilación falló.' }
$ctestPath = Join-Path (Split-Path -Parent $cmakePath) 'ctest.exe'
& $ctestPath --test-dir $buildPath -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Las pruebas fallaron.' }
Write-Output (Join-Path $buildPath "StudioSP_artefacts\$Configuration\Standalone\StudioSP.exe")
