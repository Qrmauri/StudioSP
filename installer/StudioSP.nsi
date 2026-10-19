; Build with scripts/build-installer.ps1. NSIS 3.12 Unicode.
Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "WordFunc.nsh"
!include "WinVer.nsh"
!include "x64.nsh"
!include "Sections.nsh"

Name "StudioSP ${APP_VERSION}"
OutFile "${OUTPUT_FILE}"
InstallDir "$PROGRAMFILES64\StudioSP"
!ifdef TEST_BUILD
  RequestExecutionLevel user
!else
  RequestExecutionLevel admin
  InstallDirRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\StudioSP64" "InstallLocation"
!endif
SetCompressor /SOLID lzma
SetCompressorDictSize 32
VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "StudioSP"
VIAddVersionKey /LANG=1033 "FileDescription" "StudioSP Windows x64 Installer"
VIAddVersionKey /LANG=1033 "FileVersion" "${APP_VERSION}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "StudioSP Audio"
ShowInstDetails show
ShowUninstDetails show
BrandingText "StudioSP Audio"

Var VstDir
Var MenuDir
Var DesktopDir
!ifndef TEST_BUILD
  Var RuntimeVersion
  Var Result
!endif
Var HasStandalone
Var HasVst

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Instalar StudioSP"
!define MUI_WELCOMEPAGE_TEXT "Sampler de 16 pads con secuenciador A/B/C/D, MIDI y sincronizacion al DAW.$\r$\n$\r$\nPuedes instalar la aplicacion independiente, el plugin VST3 o ambos.$\r$\n$\r$\nCierra StudioSP y tus DAW antes de continuar. El instalador incluye Visual C++ Runtime x64 de Microsoft si tu equipo lo necesita."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TEXT "StudioSP esta instalado.$\r$\n$\r$\nSi instalaste el VST3, vuelve a escanear los plugins en tu DAW. Tus proyectos y carpetas de samples se conservan."
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH
!insertmacro MUI_LANGUAGE "Spanish"
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "StudioSP necesita Windows de 64 bits." /SD IDOK
    SetErrorLevel 2
    Abort
  ${EndIf}
  ${IfNot} ${AtLeastWin10}
    MessageBox MB_OK|MB_ICONSTOP "StudioSP necesita Windows 10 o posterior." /SD IDOK
    SetErrorLevel 2
    Abort
  ${EndIf}
  SetRegView 64
  !ifdef TEST_BUILD
    StrCpy $INSTDIR "${TEST_ROOT}\App"
    StrCpy $VstDir "${TEST_ROOT}\VST3\StudioSP.vst3"
    StrCpy $MenuDir "${TEST_ROOT}\Shortcuts\Menu"
    StrCpy $DesktopDir "${TEST_ROOT}\Shortcuts\Desktop"
    SetSilent silent
  !else
    SetShellVarContext all
    StrCpy $VstDir "$COMMONFILES64\VST3\StudioSP.vst3"
    StrCpy $MenuDir "$SMPROGRAMS\StudioSP"
    StrCpy $DesktopDir "$DESKTOP"
  !endif
FunctionEnd

; All lock checks precede file changes, so an open DAW cannot cause a partial upgrade.
!macro CheckWritable FILEPATH IDENTIFIER
  retry_${IDENTIFIER}:
  ${If} ${FileExists} "${FILEPATH}"
    ClearErrors
    FileOpen $0 "${FILEPATH}" a
    ${If} ${Errors}
      MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "No se puede modificar ${FILEPATH}.$\r$\nCierra StudioSP y todos los DAW, y pulsa Reintentar." /SD IDCANCEL IDRETRY retry_${IDENTIFIER}
      SetErrorLevel 2
      Abort
    ${EndIf}
    FileClose $0
  ${EndIf}
!macroend

Section "-Preparar"
  Call CheckSelectedFiles
  !ifndef TEST_BUILD
    ; Only install Microsoft's signed runtime if an equal/newer runtime is not present.
    ReadRegStr $RuntimeVersion HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64" "Version"
    StrCpy $0 $RuntimeVersion 1
    ${If} $0 == "v"
      StrCpy $RuntimeVersion $RuntimeVersion "" 1
    ${EndIf}
    ${VersionCompare} $RuntimeVersion "${RUNTIME_VERSION}" $Result
    ReadRegDWORD $1 HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64" "Installed"
    ${If} $1 != 1
      StrCpy $Result "2"
    ${EndIf}
    ${If} $Result != 0
    ${AndIf} $Result != 1
      InitPluginsDir
      SetOutPath "$PLUGINSDIR"
      File "${PAYLOAD}\Runtime\vc_redist.x64.exe"
      ExecWait '"$PLUGINSDIR\vc_redist.x64.exe" /install /quiet /norestart' $Result
      ${If} $Result == 3010
        SetRebootFlag true
      ${ElseIf} $Result != 0
        MessageBox MB_OK|MB_ICONSTOP "No se pudo instalar Visual C++ Runtime (codigo $Result). No se instalaron los archivos de StudioSP." /SD IDOK
        SetErrorLevel $Result
        Abort
      ${EndIf}
    ${EndIf}
  !endif
  StrCpy $HasStandalone "0"
  StrCpy $HasVst "0"
  ReadINIStr $HasStandalone "$INSTDIR\installation.ini" "components" "standalone"
  ReadINIStr $HasVst "$INSTDIR\installation.ini" "components" "vst3"
SectionEnd

Section "Aplicacion independiente (Standalone)" SecStandalone
  SetOutPath "$INSTDIR"
  SetOverwrite on
  File "${PAYLOAD}\Standalone\StudioSP.exe"
  StrCpy $HasStandalone "1"
SectionEnd

Section "Plugin VST3 para DAW (64 bits)" SecVst
  SetOutPath "$VstDir"
  SetOverwrite on
  File /r "${PAYLOAD}\VST3\*.*"
  StrCpy $HasVst "1"
SectionEnd

Section "Acceso directo en el escritorio" SecDesktop
  ${If} $HasStandalone == "1"
    CreateDirectory "$DesktopDir"
    CreateShortcut "$DesktopDir\StudioSP.lnk" "$INSTDIR\StudioSP.exe"
  ${EndIf}
SectionEnd

Section "-Finalizar"
  SetOutPath "$INSTDIR"
  File /r "${PAYLOAD}\Docs\*.*"
  WriteINIStr "$INSTDIR\installation.ini" "components" "standalone" $HasStandalone
  WriteINIStr "$INSTDIR\installation.ini" "components" "vst3" $HasVst
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$MenuDir"
  ${If} $HasStandalone == "1"
    CreateShortcut "$MenuDir\StudioSP.lnk" "$INSTDIR\StudioSP.exe"
  ${EndIf}
  CreateShortcut "$MenuDir\Guia de uso.lnk" "$INSTDIR\LEEME.txt"
  CreateShortcut "$MenuDir\Desinstalar StudioSP.lnk" "$INSTDIR\Uninstall.exe"
  !ifndef TEST_BUILD
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\StudioSP64" "DisplayName" "StudioSP"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\StudioSP64" "DisplayVersion" "${APP_VERSION}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\StudioSP64" "Publisher" "StudioSP Audio"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\StudioSP64" "InstallLocation" "$INSTDIR"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\StudioSP64" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\StudioSP64" "QuietUninstallString" '$\"$INSTDIR\Uninstall.exe$\" /S'
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\StudioSP64" "URLInfoAbout" "https://github.com/Qrmauri/StudioSP"
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\StudioSP64" "NoModify" 1
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\StudioSP64" "NoRepair" 1
  !endif
SectionEnd

Function CheckSelectedFiles
  ${If} ${SectionIsSelected} ${SecStandalone}
    !insertmacro CheckWritable "$INSTDIR\StudioSP.exe" install_app
  ${EndIf}
  ${If} ${SectionIsSelected} ${SecVst}
    !insertmacro CheckWritable "$VstDir\Contents\x86_64-win\StudioSP.vst3" install_vst
  ${EndIf}
FunctionEnd

Function un.onInit
  SetRegView 64
  !ifdef TEST_BUILD
    StrCpy $VstDir "${TEST_ROOT}\VST3\StudioSP.vst3"
    StrCpy $MenuDir "${TEST_ROOT}\Shortcuts\Menu"
    StrCpy $DesktopDir "${TEST_ROOT}\Shortcuts\Desktop"
    SetSilent silent
  !else
    SetShellVarContext all
    StrCpy $VstDir "$COMMONFILES64\VST3\StudioSP.vst3"
    StrCpy $MenuDir "$SMPROGRAMS\StudioSP"
    StrCpy $DesktopDir "$DESKTOP"
  !endif
  !insertmacro CheckWritable "$INSTDIR\StudioSP.exe" uninstall_app
  !insertmacro CheckWritable "$VstDir\Contents\x86_64-win\StudioSP.vst3" uninstall_vst
FunctionEnd

Section "Uninstall"
  ReadINIStr $HasStandalone "$INSTDIR\installation.ini" "components" "standalone"
  ReadINIStr $HasVst "$INSTDIR\installation.ini" "components" "vst3"
  ${If} $HasStandalone == "1"
    Delete "$INSTDIR\StudioSP.exe"
    Delete "$MenuDir\StudioSP.lnk"
    Delete "$DesktopDir\StudioSP.lnk"
  ${EndIf}
  ${If} $HasVst == "1"
    !include "${PAYLOAD}\uninstall-vst.nsh"
  ${EndIf}
  !include "${PAYLOAD}\uninstall-docs.nsh"
  Delete "$MenuDir\Guia de uso.lnk"
  Delete "$MenuDir\Desinstalar StudioSP.lnk"
  RMDir "$MenuDir"
  !ifdef TEST_BUILD
    RMDir "$DesktopDir"
  !endif
  Delete "$INSTDIR\installation.ini"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  !ifndef TEST_BUILD
    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\StudioSP64"
  !endif
  ; No recursive deletion: projects, samples, preferences and unknown files stay untouched.
SectionEnd
