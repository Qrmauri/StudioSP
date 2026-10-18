#pragma once
#include "audio/AudioEngine.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace studiosp::ui {
class SequencerPanel : public juce::Component, private juce::Timer {
public:
    explicit SequencerPanel(audio::AudioEngine& source) : engine(source) {
        addAndMakeVisible(close); close.onClick = [this] { setVisible(false); };
        addAndMakeVisible(play); play.onClick = [this] {
            if (engine.sequencer.followsHost() ? engine.sequencer.isArmed() : engine.sequencer.isPlaying()) engine.panic();
            else engine.sequencer.start();
            refresh();
        };
        addAndMakeVisible(clock); clock.setName("Sequencer clock");
        clock.addItem("INTERNAL", 1); clock.addItem("DAW SYNC", 2);
        clock.setTooltip("DAW sigue Play/Stop, posicion y BPM del host. Sin datos del host espera en silencio.");
        clock.onChange = [this] {
            engine.panic(); engine.sequencer.setHostSync(clock.getSelectedId() == 2); refresh();
        };
        for (int p = 0; p < 4; ++p) {
            auto& button = patterns[p]; addAndMakeVisible(button);
            button.setButtonText(juce::String::charToString(static_cast<juce::juce_wchar>('A' + p)));
            button.setName("Pattern " + button.getButtonText());
            button.onClick = [this, p] { engine.sequencer.requestPattern(p); refresh(); };
        }
        for (auto* slider : {&bpm, &swing}) {
            addAndMakeVisible(slider); slider->setSliderStyle(juce::Slider::LinearHorizontal);
            slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 95, 28);
        }
        bpm.setName("BPM"); bpm.setRange(40, 240, 1); bpm.setTextValueSuffix(" BPM");
        swing.setName("Swing"); swing.setRange(0, 75, 1); swing.setTextValueSuffix(" % swing");
        bpm.onValueChange = [this] { engine.sequencer.setBpm(bpm.getValue()); };
        swing.onValueChange = [this] { engine.sequencer.setSwing(swing.getValue()); };
        for (int pad = 0; pad < 16; ++pad) for (int step = 0; step < 16; ++step) {
            auto& button = steps[pad * 16 + step]; addAndMakeVisible(button);
            button.setClickingTogglesState(true);
            button.setName("Pad " + juce::String(pad + 1) + ", paso " + juce::String(step + 1));
            button.onClick = [this, pad, step] {
                engine.sequencer.setStep(engine.sequencer.requestedPattern(), pad, step, steps[pad * 16 + step].getToggleState());
                refresh();
            };
        }
        startTimerHz(20); refresh();
    }
    void resized() override {
        close.setBounds(getWidth() - 118, 16, 100, 30); play.setBounds(18, 55, 120, 32);
        clock.setBounds(getWidth() - 280, 16, 150, 30);
        for (int p = 0; p < 4; ++p) patterns[p].setBounds(155 + p * 51, 55, 44, 32);
        bpm.setBounds(380, 55, 275, 32); swing.setBounds(675, 55, getWidth() - 693, 32);
        const int cell = (getWidth() - 130) / 16;
        for (int pad = 0; pad < 16; ++pad) for (int step = 0; step < 16; ++step)
            steps[pad * 16 + step].setBounds(112 + step * cell, 150 + pad * 30, cell - 5, 25);
    }
    void paint(juce::Graphics& g) override {
        const juce::Colour mint(0xffbcf468), muted(0xffa6b2b5);
        g.setColour(juce::Colour(0xff1c2225)); g.fillRoundedRectangle(getLocalBounds().toFloat(), 10);
        g.setColour(mint); g.setFont(18); g.drawText("SEQUENCER / 4 PATRONES", 18, 16, 380, 30, juce::Justification::centredLeft);
        const auto& seq = engine.sequencer;
        const auto letter = [](int p) { return juce::String::charToString(static_cast<juce::juce_wchar>('A' + p)); };
        g.setColour(muted); g.setFont(13);
        juce::String message = "EDITANDO " + letter(seq.requestedPattern()) + " / ";
        if (seq.isPlaying()) {
            message += "SONANDO " + letter(seq.currentPattern());
            if (seq.currentPattern() != seq.requestedPattern()) message += " / CAMBIO AL PROXIMO COMPAS";
        } else if (seq.followsHost()) message += !seq.isArmed() ? "PAUSA LOCAL / PULSA SEGUIR DAW"
            : seq.hostAvailable() ? "ESPERANDO PLAY DEL DAW" : "ESPERANDO BPM Y POSICION DEL DAW";
        else message += "STOP / PULSA LOS PASOS Y PLAY";
        g.drawText(message, 18, 95, getWidth() - 36, 24, juce::Justification::centredLeft);
        const int cell = (getWidth() - 130) / 16;
        for (int step = 0; step < 16; ++step) {
            const bool active = seq.isPlaying() && seq.currentPattern() == seq.requestedPattern() && seq.currentStep() == step;
            g.setColour(active ? mint : muted);
            g.drawText(active ? ">" + juce::String(step + 1) : juce::String(step + 1), 112 + step * cell, 124, cell - 5, 22, juce::Justification::centred);
        }
        for (int pad = 0; pad < 16; ++pad) {
            g.setColour(muted); g.drawText("PAD " + juce::String(pad + 1).paddedLeft('0', 2), 18, 150 + pad * 30, 85, 25, juce::Justification::centredLeft);
        }
        g.setColour(muted); g.setFont(12);
        g.drawText(seq.followsHost() ? "16 PASOS / 4 BEATS / BPM Y TRANSPORTE DEL DAW / STOP LOCAL: BOTON O ESPACIO"
            : "1 COMPAS / 16 PASOS / TEMPO INTERNO / STOP: BOTON O ESPACIO", 18, 645, getWidth() - 36, 22, juce::Justification::centredLeft);
        g.drawText("Gate dura un paso. Loop alterna inicio/parada. Los patrones se guardan con SAVE.", 18, 667, getWidth() - 36, 20, juce::Justification::centredLeft);
    }
private:
    audio::AudioEngine& engine;
    juce::TextButton close{"CERRAR"}, play{"PLAY"};
    std::array<juce::TextButton, 4> patterns;
    std::array<juce::TextButton, 256> steps;
    juce::Slider bpm, swing;
    juce::ComboBox clock;
    void timerCallback() override { if (isShowing()) refresh(); }
    void refresh() {
        auto& seq = engine.sequencer;
        const bool host = seq.followsHost();
        clock.setSelectedId(host ? 2 : 1, juce::dontSendNotification);
        play.setButtonText(host ? (seq.isArmed() ? "PAUSA LOCAL" : "SEGUIR DAW") : (seq.isPlaying() ? "STOP PATTERN" : "PLAY"));
        bpm.setEnabled(!host); bpm.setRange(host ? 1 : 40, host ? 1000 : 240, host ? 0.01 : 1);
        if (!bpm.isMouseButtonDown(true)) bpm.setValue(seq.effectiveBpm(), juce::dontSendNotification);
        if (!swing.isMouseButtonDown(true)) swing.setValue(seq.getSwing(), juce::dontSendNotification);
        for (int p = 0; p < 4; ++p) {
            patterns[p].setToggleState(p == seq.requestedPattern(), juce::dontSendNotification);
            patterns[p].setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff455c2b));
        }
        for (int pad = 0; pad < 16; ++pad) for (int step = 0; step < 16; ++step) {
            auto& button = steps[pad * 16 + step];
            const bool enabled = (seq.row(seq.requestedPattern(), pad) & (1u << step)) != 0;
            button.setToggleState(enabled, juce::dontSendNotification); button.setButtonText(enabled ? "X" : "-");
            button.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff455c2b));
            button.setColour(juce::TextButton::buttonColourId, juce::Colour(step % 4 == 0 ? 0xff374247 : 0xff283034));
        }
        repaint();
    }
};
}
