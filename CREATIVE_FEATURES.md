# Variaciones y rescate de improvisacion

## Alcance aprobado

Sampler para crear beats dentro de FL Studio. Cuatro variaciones por pad, con original conservado, opciones de pitch/recorte/fades/12-bit e intensidad. Escuchar antes de guardar una copia en otro pad. El rescate mantiene los ultimos 30 segundos de salida interna con efectos, permite escuchar/recortar y guardar en un pad. No captura el resto del DAW ni MIDI.

## Modulos y orden

1. `pad-variations`: genera cuatro borradores compartiendo los originales; DSP offline cancelable, sin modificar el pad fuente.
2. `recent-audio`: anillo preasignado de 32 segundos (30 disponibles y 2 de margen para copiar), con muestras atomicas para copiar mientras suena. Marca el final al pulsar el boton y usa un worker independiente para no esperar al DSP de variaciones. Nada de asignaciones/bloqueos en el callback. Reinicio al cambiar dispositivo/proyecto. Si una copia queda obsoleta se rechaza.
3. `creative-review`: panel dentro del editor para seleccion, preescucha, marcadores IN/OUT, destino y guardado con Undo. Pad vacio por defecto; confirmar sobreescritura y rechazar destinos cambiados mientras se confirma.

Los borradores y el anillo son temporales y no se serializan. Un resultado guardado es un pad normal y se incluye en los proyectos existentes. Nuevo proyecto/cierre descartan borradores y cancelan resultados pendientes. La preescucha usa el mismo mapeo de stretch/12-bit que los pads y queda fuera de capturas.

## Verificacion

- Generacion sin alterar original, limites y opciones bloqueadas, cancelacion y buffers compartidos.
- Anillo: orden, wrap, menos de 30 s, stereo, limite, copia concurrente, reset y cambio de frecuencia.
- Integracion: efectos incluidos, preescucha excluida, sin new/delete durante el callback; commit con Undo y destinos protegidos.
- Panel: render/controles en tamanos compacto y completo, variaciones y rescate, confirmar/cancelar y nuevo proyecto.
- Compilar Standalone/VST3 y ejecutar CTest con las herramientas CMake de Visual Studio, como `scripts/build.ps1`.

No se anaden dependencias, separacion de stems, MIDI retrospectivo ni sincronizacion al tempo. La RAM del anillo (aprox. 12 MiB a 48 kHz y 47 MiB a 192 kHz), la copia rescatada y los borradores quedan fuera del presupuesto del historial.
