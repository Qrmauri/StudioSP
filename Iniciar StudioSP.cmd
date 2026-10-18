@echo off
setlocal
set "STUDIOSP_EXE=%~dp0build\StudioSP_artefacts\Release\Standalone\StudioSP.exe"
if not exist "%STUDIOSP_EXE%" (
    echo No se encontro StudioSP.exe. Primero compila la app con scripts\build.ps1.
    pause
    exit /b 1
)
start "" /D "%~dp0" "%STUDIOSP_EXE%"
exit /b
