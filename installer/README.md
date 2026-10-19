# Instalador Windows x64

Generar y probar desde la raiz del proyecto:

```powershell
./scripts/build-installer.ps1
./scripts/test-installer.ps1
```

Salida: `build/dist/StudioSP-0.3.0-Setup-x64.exe` y su archivo `.sha256`.
La version se lee de CMakeLists.txt. `-SkipBuild` reutiliza los binarios Release
ya compilados/verificados; la opcion normal primero compila y ejecuta CTest.

Requisitos para generar: Visual Studio Build Tools C++, su redistribuible VC++
x64 y CMake, igual que para compilar StudioSP. El script descarga el ZIP oficial
de NSIS 3.12, verifica su SHA-256 y lo usa dentro de `build/installer-tools`,
sin instalar el compilador en Windows. `-NsisPath` admite un makensis.exe local.
Los binarios y las dependencias de JUCE/Rubber Band deben estar en el build
normal de CMake; las licencias se copian desde esos checkouts.

## Lo que instala

- Aplicacion independiente en `Program Files/StudioSP`, opcional.
- Bundle VST3 en `Common Files/VST3/StudioSP.vst3`, opcional.
- Accesos en Inicio y acceso opcional en el escritorio.
- Guia, avisos de terceros y manifiesto SHA-256 de los binarios.
- Desinstalador registrado en Aplicaciones de Windows.
- Visual C++ Runtime x64 oficial de Microsoft si falta o es anterior al
  distribuido con las herramientas que compilaron la aplicacion. Su firma
  Authenticode de Microsoft se comprueba al empaquetarlo; no se descarga nada
  durante la instalacion y no se fuerza un reinicio.

El paquete normal requiere permisos de administrador. Antes de modificar los
archivos comprueba que los componentes elegidos no estan bloqueados por una
aplicacion abierta. El desinstalador usa una lista de archivos del paquete,
sin borrado recursivo de la carpeta de aplicacion o plugin. Conserva archivos
ajenos, proyectos, samples y preferencias; tampoco elimina el runtime compartido.

## Prueba aislada

`test-installer.ps1` compila el mismo script con TEST_BUILD: solo cambia los
destinos a una carpeta unica en `build/installer-tests`, omite registro global
y runtime, y usa permisos de usuario. Comprueba hashes, accesos, actualizacion,
rechazo de un ejecutable bloqueado, desinstalacion y conservacion de archivos.
Guarda el resultado en `build/installer/test-result.json`. No sustituye una
prueba en una maquina limpia del instalador elevado ni del bootstrap de VC++.

El instalador generado no esta firmado digitalmente. El EXE y los archivos
intermedios estan bajo build, excluidos de Git. No se publica automaticamente
un release binario: antes de distribuirlo se debe resolver la licencia del
proyecto y las condiciones de JUCE/Rubber Band descritas en el README principal.

Referencias del empaquetador: [NSIS](https://nsis.sourceforge.io/Docs/),
[ZIP y SHA-256 de NSIS 3.12](https://sourceforge.net/projects/nsis/files/NSIS%203/3.12/nsis-3.12.zip/download).
