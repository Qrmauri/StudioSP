param([switch]$SkipBuild, [string]$NsisPath)
$ErrorActionPreference = 'Stop'
$projectRoot = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
if (-not $SkipBuild) { & (Join-Path $PSScriptRoot 'build.ps1') }
$versionMatch = [regex]::Match([System.IO.File]::ReadAllText((Join-Path $projectRoot 'CMakeLists.txt')), 'project\(StudioSP VERSION (\d+\.\d+\.\d+)')
if (-not $versionMatch.Success) { throw 'No se pudo leer la version de StudioSP.' }
$version = $versionMatch.Groups[1].Value
$buildRoot = Join-Path $projectRoot 'build'
$toolsRoot = Join-Path $buildRoot 'installer-tools'
$distRoot = Join-Path $buildRoot 'dist'
New-Item -ItemType Directory -Path $toolsRoot, $distRoot -Force | Out-Null

if (-not $NsisPath) {
    $NsisPath = Join-Path $toolsRoot 'nsis-3.12/makensis.exe'
    if (-not (Test-Path -LiteralPath $NsisPath)) {
        $archive = Join-Path $toolsRoot 'nsis-3.12.zip'
        $expectedHash = '56581f90db321581c5381193d796fffcf2d24b2f8fed2160a6c6a3baa67f2c4f'
        Invoke-WebRequest -Uri 'https://downloads.sourceforge.net/project/nsis/NSIS%203/3.12/nsis-3.12.zip' -OutFile $archive
        if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedHash) {
            # SourceForge can return a download page instead of the ZIP. Follow its own link once.
            $page = [System.IO.File]::ReadAllText($archive)
            $link = [regex]::Match($page, 'https://downloads\.sourceforge\.net/project/nsis/NSIS%203/3\.12/nsis-3\.12\.zip\?[^"<> ]+')
            if (-not $link.Success) { throw 'NSIS no tiene el SHA-256 esperado. No se ejecutara.' }
            Invoke-WebRequest -Uri ([System.Net.WebUtility]::HtmlDecode($link.Value)) -OutFile $archive
        }
        if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedHash) { throw 'SHA-256 de NSIS incorrecto.' }
        Expand-Archive -LiteralPath $archive -DestinationPath $toolsRoot -Force
    }
}
if (-not (Test-Path -LiteralPath $NsisPath)) { throw 'No se encontro makensis.exe.' }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$redistVersion = (Get-Content -LiteralPath (Join-Path $vsRoot 'VC/Auxiliary/Build/Microsoft.VCRedistVersion.default.txt') -Raw).Trim()
$redistRoot = Join-Path $vsRoot "VC/Redist/MSVC/$redistVersion"
$runtimePath = Join-Path $redistRoot 'vc_redist.x64.exe'
$runtimeSignature = Get-AuthenticodeSignature -LiteralPath $RuntimePath
if ($runtimeSignature.Status -ne 'Valid' -or $runtimeSignature.SignerCertificate.Subject -notmatch 'CN=Microsoft Corporation') {
    throw 'El runtime no tiene una firma valida de Microsoft.'
}
$crtDll = Get-ChildItem -LiteralPath (Join-Path $redistRoot 'x64') -Filter msvcp140.dll -Recurse | Select-Object -First 1
if (-not $crtDll) { throw 'No se pudo determinar la version minima de Visual C++ Runtime.' }
$runtimeVersion = [regex]::Match($crtDll.VersionInfo.FileVersion, '\d+\.\d+\.\d+\.\d+').Value
if (-not $runtimeVersion) { throw 'Version de Visual C++ Runtime desconocida.' }

# Unique staging directories avoid mixing files from different builds.
$stage = Join-Path $buildRoot ('installer/payload-' + [guid]::NewGuid().ToString('N'))
$appSource = Join-Path $buildRoot 'StudioSP_artefacts/Release/Standalone/StudioSP.exe'
$vstSource = Join-Path $buildRoot 'StudioSP_artefacts/Release/VST3/StudioSP.vst3'
$docs = Join-Path $stage 'Docs'
New-Item -ItemType Directory -Path (Join-Path $stage 'Standalone'), (Join-Path $stage 'VST3'), (Join-Path $stage 'Runtime'), (Join-Path $docs 'Licenses') -Force | Out-Null
Copy-Item -LiteralPath $appSource -Destination (Join-Path $stage 'Standalone/StudioSP.exe')
Copy-Item -LiteralPath (Join-Path $vstSource 'Contents') -Destination (Join-Path $stage 'VST3/Contents') -Recurse
Copy-Item -LiteralPath $RuntimePath -Destination (Join-Path $stage 'Runtime/vc_redist.x64.exe')
Copy-Item -LiteralPath (Join-Path $projectRoot 'installer/LEEME.txt') -Destination $docs
Copy-Item -LiteralPath (Join-Path $projectRoot 'README.md') -Destination $docs
$licenseFiles = @{
    'JUCE-LICENSE.md' = 'juce-src/LICENSE.md'
    'RubberBand-GPL-2.txt' = 'rubberband-src/COPYING'
    'FLAC.txt' = 'juce-src/modules/juce_audio_formats/codecs/flac/Flac Licence.txt'
    'Ogg-Vorbis.txt' = 'juce-src/modules/juce_audio_formats/codecs/oggvorbis/Ogg Vorbis Licence.txt'
    'JPEG.txt' = 'juce-src/modules/juce_graphics/image_formats/jpglib/README'
    'PNG.txt' = 'juce-src/modules/juce_graphics/image_formats/pnglib/LICENSE'
    'zlib.txt' = 'juce-src/modules/juce_core/zip/zlib/README'
    'HarfBuzz.txt' = 'juce-src/modules/juce_graphics/fonts/harfbuzz/COPYING'
    'SheenBidi.txt' = 'juce-src/modules/juce_graphics/unicode/sheenbidi/LICENSE'
    'CHOC.md' = 'juce-src/modules/juce_javascript/choc/LICENSE.md'
    'VST3-SDK.txt' = 'juce-src/modules/juce_audio_processors/format_types/VST3_SDK/LICENSE.txt'
}
foreach ($entry in $licenseFiles.GetEnumerator()) {
    Copy-Item -LiteralPath (Join-Path $buildRoot ('_deps/' + $entry.Value)) -Destination (Join-Path $docs ('Licenses/' + $entry.Key))
}
$revision = & git -C $projectRoot rev-parse HEAD
@{
    version = $version; sourceRevision = $revision; runtimeVersion = $runtimeVersion
    standaloneSha256 = (Get-FileHash -LiteralPath $appSource -Algorithm SHA256).Hash
    vst3Sha256 = (Get-FileHash -LiteralPath (Join-Path $vstSource 'Contents/x86_64-win/StudioSP.vst3') -Algorithm SHA256).Hash
    runtimeSha256 = (Get-FileHash -LiteralPath $RuntimePath -Algorithm SHA256).Hash
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $docs 'build-manifest.json') -Encoding utf8

# Generate exact uninstall lists. Never recursively remove an application or plugin directory.
function Write-UninstallList([string]$Root, [string]$DestinationVariable, [string]$Output) {
    $lines = @()
    foreach ($file in Get-ChildItem -LiteralPath $Root -File -Recurse) {
        $relative = $file.FullName.Substring($Root.Length + 1).Replace('$', '$$')
        $lines += 'Delete "' + $DestinationVariable + '\' + $relative + '"'
    }
    foreach ($folder in Get-ChildItem -LiteralPath $Root -Directory -Recurse | Sort-Object { $_.FullName.Length } -Descending) {
        $relative = $folder.FullName.Substring($Root.Length + 1).Replace('$', '$$')
        $lines += 'RMDir "' + $DestinationVariable + '\' + $relative + '"'
    }
    $lines += 'RMDir "' + $DestinationVariable + '"'
    [System.IO.File]::WriteAllLines($Output, $lines, [System.Text.UTF8Encoding]::new($false))
}
Write-UninstallList (Join-Path $stage 'VST3') '$VstDir' (Join-Path $stage 'uninstall-vst.nsh')
Write-UninstallList $docs '$INSTDIR' (Join-Path $stage 'uninstall-docs.nsh')
$installer = Join-Path $distRoot "StudioSP-$version-Setup-x64.exe"
$script = Join-Path $projectRoot 'installer/StudioSP.nsi'
& $NsisPath '/V2' "/DAPP_VERSION=$version" "/DRUNTIME_VERSION=$runtimeVersion" "/DPAYLOAD=$stage" "/DOUTPUT_FILE=$installer" $script
if ($LASTEXITCODE -ne 0) { throw 'NSIS no pudo compilar el instalador.' }
$installerHash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
($installerHash + '  ' + (Split-Path -Leaf $installer)) | Set-Content -LiteralPath ($installer + '.sha256') -Encoding ascii
@{ compiler = $NsisPath; payload = $stage; version = $version; runtimeVersion = $runtimeVersion; installer = $installer } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $buildRoot 'installer/latest.json') -Encoding utf8
Write-Output $installer
