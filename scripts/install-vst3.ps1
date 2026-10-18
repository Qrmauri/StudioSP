param([switch]$Update)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$resultPath = Join-Path $projectRoot 'build/vst3-install-result.txt'
try {
    $pluginSource = Join-Path $projectRoot 'build/StudioSP_artefacts/Release/VST3/StudioSP.vst3'
    $pluginRoot = Join-Path $env:CommonProgramFiles 'VST3'
    $pluginDestination = Join-Path $pluginRoot 'StudioSP.vst3'
    $binaryPath = 'Contents/x86_64-win/StudioSP.vst3'
    $sourceHash = (Get-FileHash -LiteralPath (Join-Path $pluginSource $binaryPath) -Algorithm SHA256).Hash
    if (Test-Path -LiteralPath $pluginDestination) {
        $installedHash = (Get-FileHash -LiteralPath (Join-Path $pluginDestination $binaryPath) -Algorithm SHA256).Hash
        if ($sourceHash -ne $installedHash) {
            if (-not $Update) { throw 'Ya existe otra version de StudioSP. Usa -Update para actualizarla.' }
            $installedBinary = Join-Path $pluginDestination $binaryPath
            try {
                $fileCheck = [System.IO.File]::Open($installedBinary, 'Open', 'ReadWrite', 'None')
                $fileCheck.Dispose()
            } catch { throw 'Cierra FL Studio y otros DAW antes de actualizar StudioSP.' }
            $backupRoot = Join-Path $projectRoot ('build/vst3-install-backup/' + [guid]::NewGuid().ToString())
            New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
            Copy-Item -LiteralPath $pluginDestination -Destination $backupRoot -Recurse
            Copy-Item -LiteralPath (Join-Path $pluginSource $binaryPath) -Destination $installedBinary -Force
            Copy-Item -LiteralPath (Join-Path $pluginSource 'Contents/Resources/moduleinfo.json') -Destination (Join-Path $pluginDestination 'Contents/Resources/moduleinfo.json') -Force
            $installedHash = (Get-FileHash -LiteralPath $installedBinary -Algorithm SHA256).Hash
            if ($sourceHash -ne $installedHash) { throw 'La verificacion de la actualizacion fallo.' }
        }
    } else {
        New-Item -ItemType Directory -Path $pluginRoot -Force | Out-Null
        Copy-Item -LiteralPath $pluginSource -Destination $pluginDestination -Recurse
        $installedHash = (Get-FileHash -LiteralPath (Join-Path $pluginDestination $binaryPath) -Algorithm SHA256).Hash
        if ($sourceHash -ne $installedHash) { throw 'La verificacion de la copia fallo.' }
    }
    "OK: $pluginDestination" | Set-Content -LiteralPath $resultPath
} catch {
    "ERROR: $($_.Exception.Message)" | Set-Content -LiteralPath $resultPath
    exit 1
}
