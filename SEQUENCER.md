# Secuenciador A/B/C/D

## Alcance autorizado
Cuatro patrones independientes de un compas 4/4, con 16 pasos de semicorchea
para cada uno de los 16 pads. BPM comun 40-240 (120 inicial); swing 0-75 %
(0 inicial). El swing retrasa los pasos pares sin cambiar la duracion del compas.
Cambiar de patron durante PLAY espera al siguiente compas. STOP detiene todo;
PLAY comienza desde el paso 1. Los pasos disparan los modos actuales de los pads;
Gate se libera al siguiente paso y Loop conserva su comportamiento toggle.

## Interfaz y persistencia
Panel SEQUENCER con rejilla, PLAY/STOP, A/B/C/D, BPM, SWING e indicador del patron
actual y el pendiente. Los patrones y ajustes se guardan en .studiosp y estado
VST3. Proyectos antiguos: patrones vacios y 120 BPM; proyectos sin selector de
reloj conservan INTERNAL. Nuevas instancias VST3/AU usan DAW SYNC; standalone
usa INTERNAL. Se conserva la seleccion de reloj en el proyecto.
NEW PROJECT restaura esos valores. No incluye grabacion en vivo, automatizacion,
Undo de pasos.

## Sincronizacion con el DAW
DAW SYNC sigue BPM, Play/Stop y PPQ del host. La rejilla conserva 16 semicorcheas
por ciclo de cuatro negras y se alinea con PPQ 0, incluso si el host usa otro
compas. Cada bloque usa la posicion absoluta del host: tempo, seek y loops no
acumulan deriva. Al entrar entre pasos espera al siguiente paso de la rejilla.
El swing sigue siendo ajustable. BPM del host admitido: 1-1000, independiente
del rango manual. Si faltan BPM/PPQ validos, espera silenciosamente.
STOP local pausa el seguimiento; SEGUIR DAW lo reactiva. Play/Stop del DAW
mantiene el seguimiento armado. Parar/saltar libera las voces con sus
transiciones existentes. INTERNAL permite tocar sin DAW y no cambia su BPM
manual al alternar entre relojes. No sincroniza automaticamente el time-stretch,
los puntos de loop de los samples ni el delay global.

## Implementacion y verificacion
C++20/JUCE existente, sin dependencias nuevas. Motor en src/audio/PatternSequencer.h,
integracion en AudioEngine, persistencia en PluginProcessor y panel JUCE separado.
Modelo atomico fijo; contador fraccional de muestras exclusivo del callback,
sin bloqueos ni asignaciones. Estilo: metodos cortos y std::array como AudioEngine.
Pruebas en tests/DspTests.cpp: tiempos exactos, swing, limites de bloque, cambios
de patron, stop/reset, persistencia compatible y callback sin asignaciones.
Playhead simulado: BPM/PPQ, seek, loops, tempos invalidos/ausentes,
eventos exactos entre divisiones MIDI y persistencia del reloj. Comprobar ademas
en un DAW real cuando se disponga de una sesion de prueba.
Compilar Standalone/VST3 y ejecutar CTest mediante ./scripts/build.ps1.
