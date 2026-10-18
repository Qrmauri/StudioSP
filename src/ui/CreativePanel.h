#pragma once
#include "WaveformComponent.h"
#include "audio/PadVariations.h"

namespace studiosp::ui {
// Drafts live only in this review panel. Nothing reaches a pad until Save is confirmed.
class CreativePanel : public juce::Component {
public:
    CreativePanel() {
        setName("Creative review"); setOpaque(true);
        for (auto* button : {&originalButton, &generateButton, &previewButton, &stopButton, &saveButton, &closeButton}) {
            addAndMakeVisible(button); button->setWantsKeyboardFocus(false);
        }
        for (int i = 0; i < 4; ++i) {
            auto& button = variants[static_cast<size_t>(i)]; addAndMakeVisible(button);
            button.setButtonText("V" + juce::String(i + 1)); button.setWantsKeyboardFocus(false);
            button.onClick = [this, i] { choose(candidates[static_cast<size_t>(i)], i); audition(); };
        }
        for (auto* toggle : {&pitch, &trim, &fades, &classic}) {
            addAndMakeVisible(toggle); toggle->setWantsKeyboardFocus(false);
        }
        for (auto* toggle : {&pitch, &trim, &fades}) toggle->setToggleState(true, juce::dontSendNotification);
        addAndMakeVisible(amount); amount.setRange(0, 100, 1); amount.setValue(50);
        amount.setSliderStyle(juce::Slider::LinearHorizontal); amount.setTextBoxStyle(juce::Slider::TextBoxRight, false, 68, 24);
        amount.setTextValueSuffix(" %"); amount.setTooltip("Intensidad de los cambios; desmarca un parametro para conservarlo.");
        addAndMakeVisible(waveform);
        waveform.onTrim = [this](int64_t first, int64_t last) {
            draft.startSample = draft.loopStartSample = first; draft.endSample = draft.loopEndSample = last; syncTrim();
        };
        for (auto* slider : {&trimIn, &trimOut}) {
            addAndMakeVisible(slider); slider->setRange(0, 100, 0.01);
            slider->setSliderStyle(juce::Slider::LinearHorizontal);
            slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 110, 24);
        }
        trimIn.setTextValueSuffix(" % IN"); trimOut.setTextValueSuffix(" % OUT");
        trimIn.onValueChange = [this] { changeTrim(true, trimIn.getValue()); };
        trimOut.onValueChange = [this] { changeTrim(false, trimOut.getValue()); };
        addAndMakeVisible(destination); destination.setName("Draft destination"); destination.setTextWhenNothingSelected("Elige un pad de destino");
        destination.onChange = [this] { saveButton.setEnabled(!busy && draft.hasSample() && destination.getSelectedId() > 0); };
        addAndMakeVisible(info); info.setColour(juce::Label::textColourId, juce::Colour(0xffbcf468));
        originalButton.onClick = [this] { choose(original); audition(); };
        generateButton.onClick = [this] { if (onGenerate) onGenerate(original, options()); };
        previewButton.onClick = [this] { audition(); };
        stopButton.onClick = [this] { if (onStop) onStop(); };
        closeButton.onClick = [this] { if (onClose) onClose(); };
        saveButton.onClick = [this] { if (onSave && destination.getSelectedId() > 0) onSave(draft, destination.getSelectedId() - 1, !variationMode); };
    }
    std::function<void(model::PadData, audio::VariationOptions)> onGenerate;
    std::function<void(model::PadData)> onPreview;
    std::function<void(model::PadData, int, bool)> onSave;
    std::function<void()> onStop, onClose;

    void showVariations(model::PadData source, int sourcePad, const std::array<model::PadData, 16>& pads) {
        variationMode = true; original = std::move(source); candidates = {};
        fillDestinations(pads, sourcePad); choose(original); setBusy(false);
        for (auto& button : variants) button.setEnabled(false);
        setMessage("El original se conserva. Elige que cambiar y pulsa GENERAR 4.");
        setVisible(true); resized();
    }
    void showRescue(model::PadData rescued, const std::array<model::PadData, 16>& pads) {
        variationMode = false; original = std::move(rescued); candidates = {};
        fillDestinations(pads, -1); choose(original); setBusy(false);
        setMessage("Ultimos " + juce::String(original.audioData.getNumSamples() / original.sampleRate, 1)
                   + " s. Ajusta IN/OUT y escucha antes de guardar.");
        setVisible(true); resized();
    }
    void setCandidates(std::array<model::PadData, 4> next) {
        candidates = std::move(next); choose(original); setBusy(false);
        for (auto& button : variants) button.setEnabled(true);
        setMessage("Pulsa ORIGINAL o V1-V4 para comparar. Guardar crea una copia en el destino elegido.");
    }
    void setBusy(bool value) {
        busy = value;
        for (auto* control : std::initializer_list<juce::Component*>{&amount, &pitch, &trim, &fades, &classic, &generateButton,
                              &originalButton, &previewButton, &trimIn, &trimOut, &waveform, &destination}) control->setEnabled(!busy);
        for (auto& button : variants) button.setEnabled(!busy && candidates[0].hasSample());
        saveButton.setEnabled(!busy && draft.hasSample() && destination.getSelectedId() > 0);
    }
    void setMessage(const juce::String& message) { info.setText(message, juce::dontSendNotification); }
    void dismiss() { setVisible(false); original = {}; draft = {}; candidates = {}; waveform.setPad({}); }
    void resized() override {
        const int w = getWidth();
        info.setBounds(22, 46, w - 44, 34);
        int x = 22;
        for (auto* toggle : {&pitch, &trim, &fades, &classic}) { toggle->setBounds(x, 86, 180, 30); x += 184; toggle->setVisible(variationMode); }
        amount.setBounds(22, 122, 400, 30); amount.setVisible(variationMode);
        generateButton.setBounds(438, 122, 165, 30); generateButton.setVisible(variationMode);
        originalButton.setBounds(22, 164, 130, 32); originalButton.setVisible(variationMode);
        for (int i = 0; i < 4; ++i) { variants[i].setBounds(166 + i * 100, 164, 90, 32); variants[i].setVisible(variationMode); }
        waveform.setBounds(22, variationMode ? 212 : 100, w - 44, variationMode ? 240 : 352);
        trimIn.setBounds(22, 468, (w - 60) / 2, 38); trimOut.setBounds(w / 2 + 8, 468, (w - 60) / 2, 38);
        previewButton.setBounds(22, 530, 150, 36); stopButton.setBounds(184, 530, 110, 36);
        destination.setBounds(310, 530, 320, 36); saveButton.setBounds(646, 530, 240, 36);
        closeButton.setBounds(w - 142, getHeight() - 60, 120, 34);
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff1c2225));
        g.setColour(juce::Colour(0xffbcf468)); g.drawRect(getLocalBounds(), 2); g.setFont(22);
        g.drawText(variationMode ? "VARIACIONES / " + juce::String(original.sampleName) : "RESCATAR IMPROVISACION",
                   22, 14, getWidth() - 44, 30, juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xff8d9a9d)); g.setFont(13);
        g.drawText("IN/OUT: arrastra los marcadores o usa las barras. Un pad ocupado requiere confirmacion.",
                   22, 592, getWidth() - 44, 24, juce::Justification::centredLeft);
    }
private:
    bool variationMode = true, busy = false;
    model::PadData original, draft;
    std::array<model::PadData, 4> candidates;
    std::array<juce::TextButton, 4> variants;
    juce::TextButton originalButton{"ORIGINAL"}, generateButton{"GENERAR 4"}, previewButton{"ESCUCHAR"},
                     stopButton{"STOP"}, saveButton{"GUARDAR EN PAD"}, closeButton{"CERRAR"};
    juce::ToggleButton pitch{"Cambiar pitch"}, trim{"Cambiar recorte"}, fades{"Cambiar fades"}, classic{"Cambiar 12-bit"};
    juce::Slider amount, trimIn, trimOut;
    juce::ComboBox destination;
    juce::Label info;
    WaveformComponent waveform;
    audio::VariationOptions options() const {
        return {pitch.getToggleState(), trim.getToggleState(), fades.getToggleState(), classic.getToggleState(),
                static_cast<float>(amount.getValue() / 100)};
    }
    void audition() { if (onPreview && draft.hasSample()) onPreview(draft); }
    void choose(model::PadData data, int alternative = -1) {
        draft = std::move(data); waveform.setPad(draft); syncTrim();
        originalButton.setToggleState(alternative < 0, juce::dontSendNotification);
        for (int i = 0; i < 4; ++i) variants[i].setToggleState(alternative == i, juce::dontSendNotification);
        setMessage(juce::String(draft.sampleName) + " / pitch " + juce::String(draft.pitchSemitones, 1)
                   + " st / " + (draft.classic12Bit ? "12-bit" : "original float"));
    }
    void syncTrim() {
        const double count = std::max(1, draft.audioData.getNumSamples());
        trimIn.setValue(100 * draft.startSample / count, juce::dontSendNotification);
        trimOut.setValue(100 * draft.endSample / count, juce::dontSendNotification);
    }
    void changeTrim(bool first, double percent) {
        WaveformViewport::moveMarker(first, static_cast<int64_t>(draft.audioData.getNumSamples() * percent / 100),
                                    draft.audioData.getNumSamples(), draft.startSample, draft.endSample);
        draft.loopStartSample = draft.startSample; draft.loopEndSample = draft.endSample;
        waveform.setPad(draft); syncTrim();
    }
    void fillDestinations(const std::array<model::PadData, 16>& pads, int exclude) {
        destination.clear(juce::dontSendNotification); int firstEmpty = 0;
        for (int i = 0; i < 16; ++i) {
            if (i == exclude) continue;
            destination.addItem("Pad " + juce::String(i + 1) + (pads[i].hasSample() ? " / ocupado" : " / vacio"), i + 1);
            if (!pads[i].hasSample() && firstEmpty == 0) firstEmpty = i + 1;
        }
        destination.setSelectedId(firstEmpty, juce::dontSendNotification);
    }
};
}
