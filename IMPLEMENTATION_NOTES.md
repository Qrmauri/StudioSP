# StudioSP: transiciones, time-stretch, chop y robustez

## Logo StudioSP

- Nuevo lockup horizontal transparente: emblema de cuatro pads con pulso de audio y wordmark STUDIO SP blanco/lima. Archivo final `assets/branding/StudioSPLogo.png`; prompts de generacion/refinado y procedencia ImageGen en `assets/branding/README.md`.
- El target JUCE `StudioSPBrand` incorpora el PNG al binario. El editor carga el recurso una vez, ajusta sus margenes transparentes y lo dibuja con el mismo escalado que el resto de la interfaz. Se reubico el subtitulo para dejar espacio al logo sin desplazar los controles.
- Standalone/VST3 compilados en Windows/Release y pruebas existentes correctas (CTest 1/1, 4,07 s). Render `build/StudioSP-editor.png` inspeccionado con logo integrado a escala compacta; transparencia del PNG comprobada. La reconstruccion emitio un aviso de CMake sobre una variable de entorno del generador, sin impedir la compilacion.

## Variaciones por pad y rescate de improvisacion

- `PadVariations.h`: cuatro borradores cancelables, intensidad y opciones de pitch/recorte/fades/12-bit. Original y buffers de audio compartidos; una conversion clasica compartida cuando hace falta. No modifica el origen.
- `RecentAudio.h`: 32 segundos preasignados de muestras atomicas lock-free, 30 recuperables y margen de dos segundos. El escritor reserva antes de escribir y publica al terminar; el lector copia fuera del callback y rechaza ventanas sobrescritas o de otra generacion. RESCATAR marca el ultimo bloque completo al pulsar y copia en un worker separado de las conversiones.
- La captura recoge mezcla con FX/master y clipper, antes de sumar la preescucha. No graba el resto del DAW. Cambios de dispositivo/proyecto invalidan marcas y descartan las colas de transicion del proyecto anterior. Solo avanza cuando el host procesa audio; no se serializa el anillo ni los borradores.
- `CreativePanel.h`: comparar ORIGINAL/V1-V4, preescucha, forma de onda con IN/OUT y barras, destino vacio por defecto y seleccion explicita cuando el banco esta lleno. El origen de variaciones no aparece entre los destinos. El guardado de rescate copia solo la seleccion en el worker.
- `commitDraft` valida revision y permiso de reemplazo antes de guardar con Undo. Las confirmaciones caducadas no sobrescriben cambios posteriores. Cerrar, abrir proyecto o NEW PROJECT invalidan los resultados pendientes del panel.
- Preescucha ahora reutiliza el mapeo de reproduccion de pads para respetar stretch y 12-bit; sigue fuera de resampleo y captura retrospectiva.
- Regresiones de original conservado, opciones bloqueadas/intensidad cero, conversion compartida/cancelacion, orden/wrap/stereo/30 s, marca al clic y rechazo de marca obsoleta, copia concurrente, efectos/preescucha, ausencia de new/delete en callback, recorte, Undo/Redo y destinos protegidos. Flujo GUI real: generar, elegir V1, guardar/deshacer, rescatar y cancelar reemplazo; capturas en `build/StudioSP-variations.png` y `build/StudioSP-rescue.png`, inspeccionadas.
- Se desactivo temporalmente la proteccion de reemplazo y la prueba fallo con `Occupied pads must require explicit overwrite approval`; se restauro el codigo y se recompilo antes de instalar.
- No se verifico dentro de FL Studio, con sanitizadores ni en Linux/macOS. El anillo consume unos 12 MiB a 48 kHz y 47 MiB a 192 kHz; copias/borradores quedan fuera del presupuesto de Undo. No se promete una certificacion de latencia o ausencia perceptual de artefactos.
- Verificacion final: compilacion Windows/Release de Standalone, VST3 y tests correcta; CTest 1/1 Passed, 4,71 s. VST3 actualizado mediante el instalador y copia instalada verificada por SHA-256 contra la compilacion final.

## Tamano inicial dentro del DAW

- El ajuste a pantalla completa no reservaba espacio para los paneles/barras del DAW. El plugin ahora abre como maximo al 70 % (1022x651 antes del escalado externo del host), mientras que standalone conserva su ajuste inicial anterior.
- Selector UI 50-200 % dentro del editor, con escalado proporcional de controles e interaccion. Tambien refleja porcentajes intermedios al redimensionar desde el host. No modifica el zoom de FL Studio.
- Regresion: el tamano inicial del plugin no supera 1022x651 y los selectores UI 50/70 % producen 730x465/1022x651. Pruebas de limites y deteccion de controles en varios tamanos y del resto de funciones correctas; Standalone/VST3 compilados. No se verifico el resultado dentro de FL Studio en esta sesion.
- `scripts/install-vst3.ps1 -Update` comprueba que el binario instalado no este en uso y guarda la version anterior bajo `build/vst3-install-backup/` antes de actualizarlo.

## Nuevo proyecto

- NEW PROJECT junto a OPEN/SAVE solicita confirmacion; Cancelar conserva la sesion. Empezar de cero vacia los 16 pads, restaura parametros/efectos, limpia Undo/Redo y cancela captura/preescucha. No elimina archivos ni cambia dispositivos o carpetas de samples.
- El reinicio se encola en el mismo worker de archivos despues de los trabajos existentes, con controles deshabilitados temporalmente y cancelacion de DSP. Invalida selectores de archivo y preescuchas pendientes. Al terminar selecciona pad 1 y restaura los controles de captura/chop.
- Pruebas de reinicio del motor (pads, efectos, historial, captura, notas y preescucha pendientes) y del dialogo con el bucle de eventos real: cancelar, volver a abrir y confirmar. Compilacion Release Windows Standalone/VST3 y CTest 1/1 correctos.

## Ventana y memoria del historial (17 de octubre de 2026)

- Ventana redimensionable entre 730x465 y 2920x1860, con proporcion 1460:930 y ajuste inicial al area util de la pantalla principal. Los controles conservan coordenadas de diseno; dibujo e interaccion usan la misma escala.
- Historial conjunto Undo/Redo limitado a 128 operaciones y 256 MiB estimados de buffers adicionales a los pads actuales. Deduplica buffers compartidos de originales, stretch, modo clasico y picos; descarta operaciones completas, incluido chop. Usa el hilo de reclamacion existente y conserva las voces activas. No limita la RAM total ni el pico de memoria de trabajos o reclamaciones pendientes.
- Regresiones verificadas: 140 ediciones conservan las ultimas 128 y su Redo; limites de memoria con buffers independientes/compartidos/derivados, chop agrupado, invalidacion de Redo y reproduccion durante reclamacion sin new/delete en el callback. La prueba de memoria falla al desactivar temporalmente ese limite; se restauro el codigo antes de la compilacion final.
- Controles dentro de la ventana y deteccion del raton comprobados a 730x465, 1460x930, 2190x1395 y 1068x680. Render final inspeccionado a 1068x680 en `build/StudioSP-editor.png`.
- Compilacion Release Windows de Standalone y VST3 correcta; CTest 1/1 correcto (4,22 s de prueba). No se verifico redimensionamiento en DAW reales ni configuraciones fisicas de DPI multiples.

## Alcance

Cambios sobre C++20 / JUCE 8.0.4, Windows, Standalone y VST3. Se mantiene la version declarada 0.3.0 y el formato de estado version 1 con 16 pads. Time-stretch offline y forma de onda/chop son las prioridades elegidas por el usuario.

## Diagnostico inicial, por impacto

1. STOP/choke/retrigger sustituian o eliminaban la voz inmediatamente. STOP tambien borraba delay/filtro de golpe. Un loop DC con fades de cero reprodujo el corte en una prueba que fallo antes del cambio.
2. Conservar una voz anterior para hacer crossfade requiere proteger otro snapshot: un pin unico por pad permitiria reclamar el audio de la cola mientras se reproduce.
3. Conversiones largas podian coincidir con ediciones. Comparar solo la identidad del audio no detecta cambios de parametros ni operaciones Undo/Redo; un resultado tardio podria reemplazarlos.
4. El historial era por pad: chop necesitaba una operacion conjunta, destinos explicitos y una previsualizacion que no copiara todo el audio.
5. La reclamacion dependia del editor y el cierre esperaba trabajos sin cancelacion DSP. La estabilidad en hosts reales no estaba certificada por las pruebas locales.

## Plan y esfuerzo orientativo

| Prioridad | Trabajo | Esfuerzo de desarrollo/validacion estimado |
|---|---|---|
| Alto | Transiciones acotadas y proteccion de snapshots | 3-5 h |
| Alto | Rubber Band offline, estado reversible y resultados obsoletos | 4-8 h |
| Medio | Chop conjunto y previsualizacion de cortes | 4-6 h |
| Medio | Bloques variables, estado antiguo, cancelacion y concurrencia | 2-4 h |
| Bajo | Escucha comparativa, pluginval y matriz de DAW/dispositivos | Pendiente; depende de hosts disponibles |

Los esfuerzos son estimaciones de trabajo y validacion, no mediciones de esta sesion.

## Cambios concretos

- `src/audio/PadSampler.h`: voz principal y cola de estado escalar/raw pointers. Crossfade lineal de 3 ms. Robo de cola con correccion desde el ultimo frame mezclado. Si la cola termina antes, su ultimo frame completa la rampa. STOP duro se reserva para reinicializacion; Gate de cero fade conserva su comportamiento.
- `src/audio/AudioEngine.h`: pins de voz/cola con orden secuencialmente consistente; suavizado de STOP sobre la mezcla para cubrir el borrado de FX; hilo de reclamacion propio para snapshots e historial retirado. Revisiones por pad para rechazar conversiones y chops obsoletos. Historial agrupado para chop; drenado de la cola SPSC como consumidor al preparar audio, sin reiniciar su indice productor.
- `src/audio/dsp/TimeStretch.h`: dos pasadas study/process/retrieve con Rubber Band R3, canales enlazados, pitch scale 1. Ratios 0,5-2 y cinco minutos maximos de salida. Bloques de 1024 y cancelacion cooperativa. Para inputs menores de 250 ms se completa una ventana con silencio al final y se recorta al numero de muestras esperado; evita que la compensacion de inicio de R3 3.3 agote un objetivo muy corto. Un resultado incompleto se rechaza.
- `src/audio/dsp/Classic12Bit.h`: cancelacion cooperativa; recibe el audio stretched si existe. Conserva el original.
- `src/model/PadData.h`: ratio y buffer derivado de stretch, separados del original y del buffer clasico.
- `src/PluginProcessor.h`: guarda ratio opcional y audio original; valida ratio al restaurar y reconstruye buffers derivados antes de publicar el estado completo. Campos ausentes conservan ratio 1, modo clasico apagado y fades 0.
- `src/ui/MainComponent.h/.cpp`: control de duracion/APPLY STRETCH; 4/8/16 cortes, destino PADS 1-N y trabajos DSP cancelables al cerrar. Resultado aplicado al siguiente disparo. Estado de procesamiento y operaciones obsoletas visibles.
- `src/ui/WaveformComponent.h`: marcas de cortes dentro de IN/OUT; zoom y recorte existentes se mantienen. La onda representa el original.
- `CMakeLists.txt`: Rubber Band 3.3.0 fijado al commit `2be46b0dffb13273a67396c77bc9278736bb03d2`, checkout local opcional y biblioteca estatica con FFT/resampler incluidos. `NOMINMAX` evita macros de Windows; Accelerate se enlaza en Apple.
- `tests/DspTests.cpp`: regresiones de transiciones, DSP offline, estado y operaciones conjuntas. Excepciones de pruebas reportadas con mensaje legible.
- `README.md` y `CONTEXT.md`: alcance, controles, compilacion y pendientes actualizados.

## Pruebas

- Reproduccion previa del corte STOP sobre un loop constante.
- Continuidad de STOP/choke/retrigger, dos estados por pad, retriggers repetidos y finalizacion acotada de las colas.
- Bloques de 1, 7, 40, 128, 256 y cero muestras; reinicializacion a 44,1 kHz.
- Auditoria de `operator new/new[]/delete` en el hilo que ejecuta el callback. No intercepta todas las variantes de asignacion ni malloc/free directos.
- Duracion exacta, pitch de tono 440 Hz y coherencia estereo a ratios 0,5/1,5/2.
- Cancelacion DSP, ratios no finitos, samples muy cortos silenciosos y no silenciosos.
- Revisiones obsoletas antes/durante el procesamiento y destinos de chop modificados.
- Original conservado, modo clasico despues del stretch, Undo/Redo y reconstruccion desde el estado.
- Estado antiguo sin ratio/modo clasico; pruebas existentes de fades ausentes y captura/preescucha aisladas.
- Chop sin huecos/solapamientos entre cortes, buffers compartidos, origen fuera y dentro de destinos y Undo/Redo conjunto.
- Pruebas existentes de mutex ocupado y publicacion/reclamacion/guardado concurrentes.
- Render del editor e inspeccion visual de los controles y marcas.

## Riesgos y limites

- Una transicion lineal de 3 ms puede modificar ataques/colas y la suma de pads distintos puede alcanzar el clipper. No certifica ausencia perceptual de clicks en todo material.
- Fin natural sin fade, Gate con fade 0 y STOP PREVIEW conservan cortes inmediatos.
- El callback no ejecuta Rubber Band ni toma el mutex de edicion. El historial ahora limita sus buffers adicionales a 256 MiB estimados y 128 operaciones; los pads actuales, voces, capturas, procesos y reclamacion pendiente quedan fuera de ese presupuesto. No hay un limite global de RAM del programa.
- La restauracion de estado del host es sincrona y reconstruye buffers derivados. Puede tardar, especialmente con 16 cortes del mismo original. No se ha convertido la API de estado a un mecanismo asincrono ni se promete seguridad de restauracion desde un hilo de tiempo real.
- Los proyectos viejos se restauran con valores por defecto. Una version vieja de StudioSP que abra un proyecto nuevo desconocera el ratio y reproducira el original.
- La cancelacion de DSP es cooperativa entre bloques. Lecturas de disco y restauracion de proyectos aun pueden demorar el cierre del editor.
- Chop sustituye pads 1-N; sus nombres y marcas lo indican. No detecta transitorios ni admite marcadores de chop manuales. No se han anadido bancos ni buses FX separados.
- La preescucha sigue fuera de efectos/capturas; la grabacion de entrada permanece seca.
- Falta validacion en DAW/dispositivos reales, pluginval, Debug/ASan y Linux/macOS. CTest no certifica todos los hosts, calidad perceptual ni latencia.
- Rubber Band requiere revisar GPL-2.0 o posterior/licencia comercial al distribuir, ademas de JUCE. Ver `COPYING` en el checkout de la dependencia.

## Verificacion ejecutada

- `./scripts/build.ps1` ejecutado en Windows/Release: configuracion, compilacion Standalone/VST3/tests y CTest correctos.
- Ultimo resultado: `StudioSP.DSP` **1/1 Passed**, 4,11 s de prueba (4,30 s total de CTest).
- Se inspecciono `build/StudioSP-editor.png`: controles de stretch/chop y marcas sin solapamientos.
- La descarga Git y la compilacion requirieron salir del sandbox: la red fallo en el entorno restringido y MSBuild quedaba detenido. La configuracion final uso el checkout local de Rubber Band 3.3.0 en `build/_deps/rubberband-src`.
- No se ejecutaron pluginval ni pruebas en DAW, Linux/macOS, Debug o sanitizadores.
# Secuenciador A/B/C/D

`PatternSequencer.h` contiene cuatro matrices de 16 mascaras atomicas de pasos.
El contador fraccional pertenece al hilo de audio y divide los tramos de render
sin crear eventos MIDI temporales ni asignar memoria. El siguiente patron se
elige en el paso 1. El swing se fija por pareja de semicorcheas; la segunda
compensa la demora de la primera para conservar la duracion de la pareja.
El panel JUCE tiene 256 pasos y comparte los colores/escalado del editor.
La propiedad binaria opcional `sequencer` conserva el formato de proyecto
version 1 y sus 16 hijos Pad; se valida antes de aplicar el estado y se restaura
siempre detenido. Proyectos antiguos usan patrones vacios y 120 BPM.
No incluye grabacion en vivo ni historial de pasos.
Pruebas cubren offsets de audio, swing, deriva fraccional a 44,1 kHz,
patrones, restauracion/legacy, limites y callback sin new/delete, ademas de
controles del panel y captura `build/StudioSP-sequencer.png`.

## DAW SYNC

`StudioSPProcessor::processBlock` consulta `AudioPlayHead::getPosition()` del
JUCE 8.0.4 local y extrae BPM, PPQ e isPlaying, siempre dentro del callback.
`beginBlock` detecta discontinuidades y el render usa PPQ absoluto en cada
tramo, incluso cuando los eventos MIDI dividen el bloque. Los limites de paso
se redondean hacia la siguiente muestra; el swing retrasa el segundo paso de
cada pareja. STOP, seek, loop o cambio de reloj liberan las voces con la
transicion existente. El estado opcional `sequencerHostSync` guarda la eleccion;
proyectos antiguos usan INTERNAL y nuevas instancias plugin usan DAW SYNC.
El host no modifica el BPM interno guardado. Sin datos validos espera; STOP
local desarma el seguimiento hasta SEGUIR DAW. Pruebas con playhead simulado
cubren transportes, tempos, offsets, seeks/loops, ausencia de datos,
continuidad de bloques divididos, persistencia y callback sin asignaciones.
No se ha verificado esta funcion dentro de un DAW real.

Verificacion Windows/Release: Standalone y VST3 compilados; CTest StudioSP.DSP
1/1 Passed (4,25 s). Panel DAW SYNC inspeccionado en captura y controles
comprobados a escala 50/100 %. Imagen para GitHub en
`assets/screenshots/StudioSP-sequencer.png`.

