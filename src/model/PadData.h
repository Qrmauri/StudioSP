#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <string>
#include "SampleBuffer.h"

namespace studiosp::model {

enum class PlaybackMode {
    OneShot, // Dispara y reproduce hasta el final (o hasta que se active otro pad del mismo choke group)
    Gate,    // Reproduce solo mientras el pad/tecla se mantenga presionado
    Loop     // Se repite indefinidamente hasta presionar de nuevo o stop
};

enum class FxBusTarget {
    Dry,     // Salida directa sin efectos
    Bus1,    // Dirigido a la cadena FX Bus 1 (ej. Filtro/Delay)
    Bus2     // Dirigido a la cadena FX Bus 2 (ej. Saturador/Reverb)
};

/**
 * Representa los datos y el estado de audio de un Pad individual.
 * Todas las muestras se almacenan en 32-bit floating point para máxima fidelidad.
 */
struct PadData {
    int padIndex = 0; // 0 a 15
    std::string sampleName = "Empty";
    
    // Buffer de audio en 32-bit float estéreo
    SampleBuffer audioData;
    double sampleRate = 48000.0;
    bool classic12Bit = false;
    SampleBuffer classicAudioData;
    double stretchRatio = 1.0; // Output duration / original duration; pitch is unchanged.
    SampleBuffer stretchedAudioData;

    // Puntos de reproducción (en muestras)
    int64_t startSample = 0;
    int64_t endSample = 0;
    int64_t loopStartSample = 0;
    int64_t loopEndSample = 0;

    // Propiedades de modulación y mezcla
    float volume = 1.0f;       // 0.0f a 1.0f (o hasta 2.0f con boost)
    float pan = 0.0f;          // -1.0f (Izquierda) a +1.0f (Derecha)
    float pitchSemitones = 0.0f; // -24.0 a +24.0 semitonos
    float fadeInMs = 2.0f;
    float fadeOutMs = 3.0f;
    
    // Configuración de reproducción estilo SP
    PlaybackMode mode = PlaybackMode::OneShot;
    FxBusTarget busTarget = FxBusTarget::Dry;
    int chokeGroup = 0;        // 0 = sin mute; 1..8 = se apagan entre sí (ej. Hi-Hat Open/Closed)
    
    bool hasSample() const {
        return audioData.getNumSamples() > 0 && audioData.getNumChannels() > 0;
    }
};

} // namespace studiosp::model
