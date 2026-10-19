$ErrorActionPreference = 'Stop'
$projectRoot = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$config = Get-Content -LiteralPath (Join-Path $projectRoot 'build/installer/latest.json') -Raw | ConvertFrom-Json
$testRoot = [System.IO.Path]::GetFullPath((Join-Path $projectRoot ('build/installer-tests/' + [guid]::NewGuid().ToString('N'))))
$allowedRoot = [System.IO.Path]::GetFullPath((Join-Path $projectRoot 'build/installer-tests')) + [System.IO.Path]::DirectorySeparatorChar
if (-not $testRoot.StartsWith($allowedRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Destino de prueba fuera de build/installer-tests.' }
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
$testExe = Join-Path $testRoot 'Setup-test.exe'
& $config.compiler '/V2' '/DTEST_BUILD' "/DTEST_ROOT=$testRoot" "/DAPP_VERSION=$($config.version)" "/DRUNTIME_VERSION=$($config.runtimeVersion)" "/DPAYLOAD=$($config.payload)" "/DOUTPUT_FILE=$testExe" (Join-Path $projectRoot 'installer/StudioSP.nsi')
if ($LASTEXITCODE -ne 0) { throw 'No se pudo compilar la variante aislada de prueba.' }
function Invoke-TestSetup {
    $process = Start-Process -FilePath $testExe -ArgumentList '/S' -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw ('La instalacion aislada fallo: ' + $process.ExitCode) }
}
Invoke-TestSetup
$appDir = Join-Path $testRoot 'App'
$app = Join-Path $appDir 'StudioSP.exe'
$vst = Join-Path $testRoot 'VST3/StudioSP.vst3/Contents/x86_64-win/StudioSP.vst3'
$manifest = Get-Content -LiteralPath (Join-Path $appDir 'build-manifest.json') -Raw | ConvertFrom-Json
if ((Get-FileHash -LiteralPath $app -Algorithm SHA256).Hash -ne $manifest.standaloneSha256) { throw 'Standalone instalado no coincide con la compilacion.' }
if ((Get-FileHash -LiteralPath $vst -Algorithm SHA256).Hash -ne $manifest.vst3Sha256) { throw 'VST3 instalado no coincide con la compilacion.' }
foreach ($file in @('LEEME.txt', 'Licenses/JUCE-LICENSE.md', 'Licenses/RubberBand-GPL-2.txt', 'Uninstall.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $appDir $file))) { throw ('Falta archivo instalado: ' + $file) }
}
$projectToPreserve = Join-Path $appDir 'Mi-proyecto.studiosp'
$pluginNoteToPreserve = Join-Path $testRoot 'VST3/StudioSP.vst3/nota-del-usuario.txt'
'proyecto de prueba que debe conservarse' | Set-Content -LiteralPath $projectToPreserve
'archivo ajeno al instalador' | Set-Content -LiteralPath $pluginNoteToPreserve

# A blocked executable must abort before replacing files, then allow an upgrade once closed.
$lockedFile = [System.IO.File]::Open($app, 'Open', 'Read', 'Read')
try {
    $blocked = Start-Process -FilePath $testExe -ArgumentList '/S' -WindowStyle Hidden -Wait -PassThru
    if ($blocked.ExitCode -eq 0) { throw 'El instalador no detecto el ejecutable bloqueado.' }
} finally { $lockedFile.Dispose() }
Invoke-TestSetup
if (-not (Test-Path -LiteralPath $projectToPreserve)) { throw 'La actualizacion elimino el proyecto.' }

$uninstaller = Join-Path $appDir 'Uninstall.exe'
$uninstall = Start-Process -FilePath $uninstaller -ArgumentList '/S' -WindowStyle Hidden -Wait -PassThru
if ($uninstall.ExitCode -ne 0) { throw ('La desinstalacion aislada fallo: ' + $uninstall.ExitCode) }
$deadline = [DateTime]::UtcNow.AddSeconds(20)
while (((Test-Path -LiteralPath $app) -or (Test-Path -LiteralPath $vst)) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
if ((Test-Path -LiteralPath $app) -or (Test-Path -LiteralPath $vst)) { throw 'Quedan binarios despues de desinstalar.' }
if (-not (Test-Path -LiteralPath $projectToPreserve) -or -not (Test-Path -LiteralPath $pluginNoteToPreserve)) { throw 'La desinstalacion elimino archivos del usuario.' }
if (Test-Path -LiteralPath (Join-Path $testRoot 'Shortcuts/Menu/StudioSP.lnk')) { throw 'Queda el acceso directo despues de desinstalar.' }
@{ passed = $true; testRoot = $testRoot; installer = $config.installer; tested = @('payload hashes', 'upgrade', 'locked executable', 'uninstall', 'preserve user files');
    scope = 'Same NSIS payload/sections, user-level test build; no global registry, VST3 or Microsoft runtime installation.' } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $projectRoot 'build/installer/test-result.json') -Encoding utf8
Write-Output 'Instalacion, actualizacion, bloqueo, desinstalacion y conservacion de archivos: OK.'
