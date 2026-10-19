# Grabacion MIDI y exportacion de patrones

REC MIDI arma overdub del patron que suena, tanto con INTERNAL como DAW SYNC.
No arranca el transporte ni vacia el patron. Captura notas MIDI 36-51 y golpes
de los pads con raton/teclado mientras el reloj avanza; note-on conserva velocidad y note-off
la duracion (Gate). STOP local, cambio de reloj, cargar o NEW desarman REC.
Los eventos del raton/teclado se capturan al inicio del siguiente bloque;
la entrada MIDI conserva el offset dentro del bloque. Preescucha del buscador
y reproduccion del propio secuenciador nunca se vuelven a grabar.
Quantize 1/16 coloca el golpe en la semicorchea mas cercana con el swing actual.
Desactivarlo conserva posicion en una rejilla fina de 9600 ticks por negra.
Una celda conserva un golpe por pad: repetir en esa celda reemplaza ese golpe.
Los golpes se escuchan inmediatamente y no se duplican durante el mismo ciclo.
Si hay cambio A/B/C/D pendiente se graba en el patron que esta sonando.

EXPORT MIDI guarda el patron elegido, un ciclo de cuatro negras, notas 36-51
en canal 10, velocidades, timing/swing, duraciones, tempo actual y compas 4/4.
Los note-off del archivo se limitan al final del ciclo para no dejar notas
abiertas al importarlo. No incluye samples ni efectos. La escritura ocurre
fuera del callback y en segundo plano; no se anaden dependencias.

Se mantienen las propiedades anteriores de los proyectos. Una propiedad
binaria opcional guarda timing, velocidad y duracion; proyectos antiguos
mantienen pasos rectos/con swing, velocidad 127 y Gate de un paso.
REC nunca se restaura armado. La cuantizacion elegida si se conserva.
Modelo de tamano fijo, atomicos para edicion/captura y estado de reloj exclusivo
del callback. Pruebas en tests/MidiTests.cpp y tests/DspTests.cpp; ./scripts/build.ps1 compila
Standalone/VST3 y ejecuta CTest. Verificar tambien el panel renderizado.
