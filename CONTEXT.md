# Contexto del proyecto StudioSP

Documento para compartir con otra IA o un nuevo chat. Preparado el 10 de octubre de 2026 a partir del README, la especificacion y el codigo local. Tras los cambios descritos abajo, se ejecuto `scripts/build.ps1` en Windows/Release: Standalone/VST3 compilados y CTest 1/1 correcto. No se validaron DAW reales ni otras plataformas.

Actualizacion posterior: se implementaron transiciones de STOP/choke/retrigger, time-stretch offline Rubber Band R3 y chop de partes iguales con previsualizacion. Consulta el README actualizado y `IMPLEMENTATION_NOTES.md` para el alcance y la verificacion de estos cambios. Las listas originales de pendientes de este documento quedan corregidas por esta actualizacion.

## Qué es

StudioSP es un sampler de escritorio inspirado en el flujo de los Roland SP: cargar samples, disparar 16 pads, editar reproducción, aplicar efectos y grabar la mezcla interna o una entrada de audio.

Es un proyecto nativo de audio en **C++20 y JUCE**, aunque la carpeta contenedora se llame `ejercicio_front_end`. No es una aplicación web.

- Versión declarada en CMake: **0.3.0**.
- JUCE descargado por CMake: **8.0.4**; se puede usar una copia local.
- Formatos: Standalone y VST3; AU se añade en macOS.
- Entorno actual: Windows y PowerShell.
- Ruta actual: `C:\Users\mauri\ejercicios\clases phyton\ejercicio_front_end\studiosp`.

## Fuentes de contexto

- `README.md`: funciones implementadas, uso, compilación y limitaciones documentadas.
- `SPECIFICATION.md`: visión original y objetivos; contiene funciones pendientes, afirmaciones de calidad aspiracionales y una ruta antigua. No debe tratarse como descripción exacta de lo implementado.
- Código fuente y `CMakeLists.txt`: comprobar aquí el comportamiento concreto antes de modificarlo.

## Estado funcional documentado

- Interfaz oscura con 16 pads, indicadores de reproducción y forma de onda.
- Carga WAV, FLAC y AIFF por arrastre o selector; audio float, hasta 192 kHz y cinco minutos por archivo.
- Disparo con ratón, teclado y MIDI; notas MIDI 36–51 para pads 1–16, con velocidad y eventos dentro del bloque.
- Modos One Shot, Gate y Loop; ocho grupos de choke.
- Pitch de ±24 semitonos con interpolación Sinc Blackman de 32 puntos y conversión de frecuencia archivo/host. Pitch modifica la duración.
- Volumen, panorama, recorte y fades de 0–100 ms por pad. El recorte también define el loop.
- Forma de onda con picos min/max, zoom, desplazamiento y marcadores IN/OUT.
- Filtro paso bajo, drive y delay global de 375 ms con feedback.
- Resampleo interno estéreo float antes de la protección de salida, hasta 60 segundos.
- Grabación de interfaz seca: entrada 1 mono, entrada 2 mono o entradas 1+2 estéreo, hasta 60 segundos. Sin monitorización por altavoces desde la aplicación.
- Buscador de samples locales con varias carpetas, indexación recursiva en segundo plano y filtro por nombre/ruta. Conserva las carpetas entre sesiones.
- Preescucha independiente de pads, efectos y capturas; nivel fijo de 0,65.
- Modo opcional por pad de 12 bits lineales a 26,04 kHz. Conserva el original y utiliza un buffer float cuantizado; conversión en segundo plano.
- Undo/Redo para carga, borrado, resampleo y parámetros de pad. Los efectos globales no tienen historial.
- Ventana redimensionable (50-200 %) con escala proporcional y ajuste inicial a la pantalla principal. El historial tiene un maximo conjunto de 128 operaciones y 256 MiB estimados de buffers adicionales, con deduplicacion y reclamacion fuera del callback; no limita la RAM total.
- Proyectos `.studiosp` y estado del plugin con audio incorporado.
- Transiciones de 3 ms en STOP/choke/retrigger/Loop, con una cola preasignada por pad y pins adicionales. Reclamacion de snapshots e historial retirado en un hilo propio.
- Time-stretch offline Rubber Band 3.3.0 R3, ratio 0,5-2, con original conservado, Undo y reconstruccion al restaurar estado; salida limitada a cinco minutos.
- Chop de IN/OUT en 4/8/16 partes iguales hacia pads 1-N, previsualizacion de marcas y una sola operacion Undo/Redo.

## Mapa del código

| Archivo | Responsabilidad |
|---|---|
| `CMakeLists.txt` | Dependencia JUCE, plugin, formatos y pruebas |
| `src/main.cpp` | Integración y creación del procesador JUCE |
| `src/PluginProcessor.h` | Procesador, buses, callback y serialización/restauración del estado |
| `src/audio/AudioEngine.h` | MIDI, mezcla, efectos, capturas, publicación de pads e historial |
| `src/audio/PadSampler.h` | Voces y reproducción de samples |
| `src/audio/dsp/SincInterpolator.h` | Interpolación y reconstrucción para pitch/remuestreo |
| `src/audio/dsp/Classic12Bit.h` | Conversión del modo clásico de 12 bits / 26,04 kHz |
| `src/audio/dsp/TimeStretch.h` | Adaptador Rubber Band offline, dos pasadas y cancelacion cooperativa |
| `src/model/PadData.h` | Datos y parámetros de cada pad |
| `src/model/SampleBuffer.h` | Buffers compartidos, copia al editar y picos de forma de onda |
| `src/model/SampleLibrary.h` | Biblioteca e indexación de samples locales |
| `src/model/SampleFolderSettings.h` | Persistencia de carpetas de samples |
| `src/ui/MainComponent.h` y `.cpp` | Editor, pads, controles y trabajos de archivos |
| `src/ui/WaveformComponent.h` | Dibujo e interacción con la forma de onda |
| `src/ui/WaveformViewport.h` | Vista, zoom y límites de marcadores |
| `src/ui/SampleBrowser.h` | Interfaz del buscador local |
| `tests/DspTests.cpp` | Pruebas de DSP, estado, concurrencia y editor |
| `scripts/build.ps1` | Configurar, compilar y ejecutar pruebas en Windows |
| `Iniciar StudioSP.cmd` | Abrir el ejecutable Release ya compilado |

## Restricciones que deben conservarse al editar

- El callback de audio reproduce snapshots inmutables y no toma el bloqueo de edición ni libera samples.
- Cargar, editar, borrar o deshacer un pad afecta al siguiente disparo. Las voces activas conservan su versión hasta terminar o recibir STOP/choke.
- Carga/guardado, carga de samples y finalización de capturas usan trabajo en segundo plano. La liberación de buffers ocurre fuera del hilo de audio.
- No introducir bloqueos de edición, operaciones de disco ni trabajo pesado de conversión dentro del callback.
- Mantener la compatibilidad al restaurar proyectos antiguos: modo clásico desactivado y fades de 0 ms cuando esas propiedades no existen.
- La preescucha no debe incorporarse al resampleo ni a la grabación de entrada.
- El modo clásico debe conservar el audio original para poder desactivarlo o deshacerlo.

## Compilación y comprobación

Requisitos Windows: Visual Studio o Build Tools con Desarrollo para el escritorio con C++, Windows SDK, Git y CMake 3.22 o superior.

Desde la raíz del proyecto:

```powershell
./scripts/build.ps1
```

El script busca CMake en PATH o en Visual Studio, configura `build`, compila Release y ejecuta CTest. Alternativa manual:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

Para JUCE local, añadir `-DJUCE_SOURCE_DIR=/ruta/a/JUCE` a la configuración. La primera configuración descarga JUCE si no se proporciona una copia local.

Salidas esperadas en Windows:

- `build/StudioSP_artefacts/Release/Standalone/StudioSP.exe`
- `build/StudioSP_artefacts/Release/VST3/StudioSP.vst3`

Las pruebas documentadas cubren DSP, MIDI, choke, capturas, historial, proyectos, concurrencia, buffers compartidos, fades, zoom y marcadores. También renderizan el editor en `build/StudioSP-editor.png`. Comprobar los resultados actuales antes de afirmar que pasan.

## Uso y controles

| Teclas | Pads |
|---|---|
| Z X C V | 1–4 |
| A S D F | 5–8 |
| Q W E R | 9–12 |
| 1 2 3 4 | 13–16 |

- Espacio: detener voces y limpiar delay.
- NEW PROJECT: pide confirmacion, vacia los pads y restaura efectos e historial. Cancela capturas/preescucha y se ejecuta despues de los trabajos de archivo pendientes. Conserva archivos guardados, carpetas y configuracion de dispositivos.
- UI: selector de escala 50-200 %. El VST3 abre como maximo al 70 % para dejar espacio a paneles y barras del DAW; standalone conserva el ajuste a pantalla. El host puede aplicar zoom adicional o restaurar su propio tamano anterior.
- Logo nuevo en `assets/branding/StudioSPLogo.png`: cuatro pads/pulso, wordmark blanco y lima, PNG transparente generado con ImageGen. Se incorpora al EXE/VST3 con el target `StudioSPBrand`; al cargar se ajustan los margenes transparentes para que el logo escale con el editor. Prompt y origen en `assets/branding/README.md`.
- VARIACIONES: cuatro borradores por pad con pitch/recorte/fades/12-bit e intensidad, original conservado y excluido del destino. El panel permite comparar, recortar y guardar una copia con Undo.
- RESCATAR / 30 s: anillo preasignado de salida interna con FX y clipper, sin preescucha. Marca el momento del clic y copia en un worker independiente. Panel con IN/OUT y preescucha; al guardar copia solo la seleccion. Pad vacio por defecto; reemplazos confirmados y protegidos por revision.
- Nuevos modulos: `src/audio/RecentAudio.h`, `src/audio/PadVariations.h`, `src/ui/CreativePanel.h`. Especificacion y alcance en `CREATIVE_FEATURES.md`. Anillo/borradores temporales no serializados, fuera del presupuesto de historial; reinicio al cambiar dispositivo o proyecto.
- Ctrl+Z / Ctrl+Shift+Z: deshacer / rehacer.
- RESAMPLE → seleccionar destino → tocar pads → FINISH REC.
- REC INPUT graba al pad seleccionado; STOP REC o STOP finaliza.
- Los pads empiezan vacíos; no incluye un kit de samples.
- Las carpetas locales se guardan en `StudioSP/sample-folders.json` bajo los datos del usuario, compartidas entre Standalone y plugin.

## Pendientes y limitaciones

- No hay time-stretch en vivo/sincronizado al host, CLAP, multiples bancos, chop por transitorios/cortes manuales ni automatizacion de parametros DAW. Secuenciador A/B/C/D con 16 pasos por pad, BPM interno 40-240 y swing 0-75 %, cambio al siguiente ciclo y persistencia. DAW SYNC sigue BPM/Play/Stop/PPQ y realinea seeks/loops, con espera silenciosa sin playhead valido. Nuevas instancias plugin: DAW; standalone y proyectos antiguos: INTERNAL. Mantiene ciclos de cuatro negras. REC MIDI captura velocidad, timing y duracion, con Quantize 1/16 opcional; EXPORT MIDI guarda un patron con tempo/swing y notas 36-51, canal 10. No tiene Undo de patrones. Ver SEQUENCER.md y MIDI_FEATURES.md.
- `FxBusTarget` existe como metadato; los efectos actuales son globales y no hay buses FX independientes.
- La salida usa un clipper de picos de muestras a −0,1 dBFS. No hay un limitador true-peak ni certificación BS.1770/−0,1 dBTP.
- La interpolación reduce aliasing, pero no garantiza ausencia total; drive no está sobremuestreado.
- Float y resampleo interno no garantizan operaciones sin pérdidas ni headroom infinito.
- STOP/choke/retrigger usan transiciones de 3 ms. Fin natural y Gate con fades de 0 ms, y STOP PREVIEW, pueden cortar abruptamente.
- Bajo publicación continua, un disparo puede descartarse tras cuatro intentos para mantener acotado el callback.
- Finalizar una captura requiere confirmación del siguiente bloque de audio. Con el host detenido puede quedar pendiente; cambiar dispositivo/frecuencia cancela la captura pendiente.
- No se garantiza latencia menor a 2,5 ms ni estabilidad en todos los hosts.
- Revisar la licencia de JUCE antes de distribuir.

## Cómo continuar en otro chat

- Instalador Windows x64 NSIS en `installer/StudioSP.nsi`. `scripts/build-installer.ps1` compila/empaqueta y `scripts/test-installer.ps1` prueba instalacion/actualizacion/desinstalacion en carpeta aislada, sin registro global ni runtime. EXE en `build/dist`, excluido de Git. Incluye Standalone/VST3 opcionales, runtime firmado de Microsoft, guia, licencias y desinstalador. Detalles y limites en `installer/README.md`.

Usa este documento como contexto inicial y añade la tarea concreta que quieres realizar. Antes de editar, revisa los archivos relevantes y verifica qué está implementado. Mantén el flujo de sampler de 16 pads, el trabajo pesado fuera del hilo de audio y la compatibilidad del estado guardado. Al terminar, indica los archivos cambiados, las comprobaciones ejecutadas y cualquier limitación pendiente.
