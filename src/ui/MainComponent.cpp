#include "MainComponent.h"
#include "audio/PatternMidi.h"
#include "StudioSPBrand.h"
#include <limits>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

namespace studiosp::ui {
namespace {
const juce::Colour background(0xff111416), panel(0xff1c2225), muted(0xff8d9a9d), mint(0xffbcf468), orange(0xffff9260);
void text(juce::Graphics& g, const juce::String& value, juce::Rectangle<int> bounds, float size, juce::Colour colour, int alignment = juce::Justification::centredLeft) {
    g.setColour(colour); g.setFont(size); g.drawText(value, bounds, alignment);
}
juce::Image loadBrandLogo() {
    auto image = juce::ImageFileFormat::loadFrom(StudioSPBrand::StudioSPLogo_png, StudioSPBrand::StudioSPLogo_pngSize);
    if (!image.isValid()) return image;
    int left = image.getWidth(), top = image.getHeight(), right = -1, bottom = -1;
    {
        const juce::Image::BitmapData pixels(image, juce::Image::BitmapData::readOnly);
        for (int y = 0; y < image.getHeight(); ++y) for (int x = 0; x < image.getWidth(); ++x)
            if (pixels.getPixelColour(x, y).getAlpha() > 16) {
                left = std::min(left, x); top = std::min(top, y); right = std::max(right, x); bottom = std::max(bottom, y);
            }
    }
    // Fit the visible artwork, excluding the export's transparent canvas margins.
    return right >= left ? image.getClippedImage({left, top, right - left + 1, bottom - top + 1}) : image;
}
}
MainComponent::MainComponent(StudioSPProcessor& p) : AudioProcessorEditor(p), engine(p.engine), processor(p) {
    brandLogo = loadBrandLogo();
    look.setColour(juce::TextButton::buttonColourId, panel);
    look.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    look.setColour(juce::ComboBox::backgroundColourId, panel);
    look.setColour(juce::Slider::rotarySliderFillColourId, mint);
    look.setColour(juce::Slider::thumbColourId, mint);
    look.setColour(juce::Slider::trackColourId, mint);
    setLookAndFeel(&look); setWantsKeyboardFocus(true);
    for (int i = 0; i < 16; ++i) { pads[i] = std::make_unique<PadComponent>(*this, i); addAndMakeVisible(*pads[i]); }
    for (auto* button : {&loadButton, &saveButton, &openButton, &resampleButton, &stopButton, &undoButton, &redoButton, &clearButton, &audioButton, &newProjectButton, &variationsButton, &rescueButton}) {
        addAndMakeVisible(button); button->setWantsKeyboardFocus(false);
    }
    loadButton.onClick = [this] { chooseFile(0); }; saveButton.onClick = [this] { chooseFile(1); }; openButton.onClick = [this] { chooseFile(2); };
    newProjectButton.onClick = [this] { requestNewProject(); };
    newProjectButton.setTooltip("Vaciar los 16 pads y restaurar ajustes para empezar otro proyecto.");
    addAndMakeVisible(creativePanel); creativePanel.setVisible(false);
    addAndMakeVisible(sequencerPanel); sequencerPanel.setVisible(false);
    sequencerPanel.onExport = [this](audio::PatternSequencer::State state, int pattern, double tempo) {
        exportPatternMidi(std::move(state), pattern, tempo);
    };
    addAndMakeVisible(sequencerButton); sequencerButton.setWantsKeyboardFocus(false);
    sequencerButton.onClick = [this] {
        closeCreativePanel(); sequencerPanel.setVisible(!sequencerPanel.isVisible());
        if (sequencerPanel.isVisible()) sequencerPanel.toFront(false);
    };
    variationsButton.onClick = [this] { openVariations(); };
    rescueButton.onClick = [this] { rescueImprovisation(); };
    variationsButton.setTooltip("Crear y comparar cuatro alternativas sin modificar el pad original.");
    rescueButton.setTooltip("Escuchar, recortar y guardar los ultimos 30 segundos de StudioSP, con efectos.");
    creativePanel.onClose = [this] { closeCreativePanel(); };
    creativePanel.onStop = [this] { ++previewRequest; engine.stopPreview(); };
    creativePanel.onPreview = [this](model::PadData data) { ++previewRequest; engine.stopPreview(); engine.setPreview(std::move(data)); };
    creativePanel.onGenerate = [this](model::PadData data, audio::VariationOptions options) { generateVariations(std::move(data), options); };
    creativePanel.onSave = [this](model::PadData data, int destination, bool crop) { saveCreativeDraft(std::move(data), destination, crop); };
    addAndMakeVisible(interfaceSize); interfaceSize.setName("Interface scale");
    interfaceSize.setWantsKeyboardFocus(false);
    interfaceSize.setTooltip("Tamano de la interfaz. Reduce el porcentaje si no cabe dentro del DAW.");
    for (int percent : {50, 60, 70, 80, 90, 100, 125, 150, 200})
        interfaceSize.addItem("UI " + juce::String(percent) + "%", percent);
    interfaceSize.onChange = [this] {
        const int percent = interfaceSize.getSelectedId();
        if (percent > 0) setSize(juce::roundToInt(designWidth * percent / 100.0),
                                 juce::roundToInt(designHeight * percent / 100.0));
    };
    resampleButton.onClick = [this] { toggleResample(); };
    stopButton.onClick = [this] { ++previewRequest; engine.stopPreview(); engine.panic(); if (engine.hasCapture()) engine.requestStopResampling(); armed = false; };
    undoButton.onClick = [this] { engine.history(false); refreshSelection(); };
    redoButton.onClick = [this] { engine.history(true); refreshSelection(); };
    clearButton.onClick = [this] { engine.replacePad(selected, {}); refreshSelection(); };
    audioButton.onClick = [] { if (auto* holder = juce::StandalonePluginHolder::getInstance()) holder->showAudioSettingsDialog(); };
    audioButton.setVisible(juce::JUCEApplicationBase::isStandaloneApp());
    addAndMakeVisible(sampleBrowser); sampleBrowser.onLoad = [this](const juce::File& file) { load(selected, file); };
    sampleBrowser.onError = [this](const juce::String& error) { status.setText(error, juce::dontSendNotification); };
    sampleBrowser.onStopPreview = [this] { ++previewRequest; engine.stopPreview(); };
    sampleBrowser.onPreview = [this](const juce::File& file) {
        const auto request = ++previewRequest; engine.stopPreview();
        status.setText("Preparando preescucha: " + file.getFileName(), juce::dontSendNotification);
        fileJobs.addJob([safe = juce::Component::SafePointer<MainComponent>(this), request, file] {
            model::PadData data; juce::String error;
            const bool loaded = audio::AudioEngine::readSampleFile(file, data, error);
            juce::MessageManager::callAsync([safe, request, file, error, loaded, data = std::move(data)]() mutable {
                if (!safe || safe->previewRequest != request) return;
                if (loaded) { safe->engine.setPreview(std::move(data)); safe->status.setText("Preescucha: " + file.getFileName(), juce::dontSendNotification); }
                else safe->status.setText(error, juce::dontSendNotification);
            });
        });
    };
    addAndMakeVisible(classicMode); classicMode.setWantsKeyboardFocus(false);
    addAndMakeVisible(stretch); stretch.setRange(0.5, 2, 0.01); stretch.setValue(1);
    stretch.setSliderStyle(juce::Slider::LinearHorizontal); stretch.setTextBoxStyle(juce::Slider::TextBoxRight, false, 90, 26);
    stretch.setTextValueSuffix(" x duration"); stretch.setTooltip("Duracion respecto al original. Conserva pitch; APPLY procesa en segundo plano.");
    addAndMakeVisible(stretchButton); stretchButton.setWantsKeyboardFocus(false);
    stretchButton.onClick = [this] {
        const int pad = selected; const double ratio = stretch.getValue(); const auto revision = engine.getRevisions()[pad];
        processingSample = true; refreshSelection();
        status.setText("Procesando stretch. El original se conserva; el siguiente disparo usara el resultado.", juce::dontSendNotification);
        fileJobs.addJob([this, safe = juce::Component::SafePointer<MainComponent>(this), pad, ratio, revision] {
            bool changed = false; juce::String message;
            try { changed = engine.setStretchRatio(pad, ratio, [this] { return cancelProcessing.load(); }, revision); }
            catch (const std::exception& error) { message = error.what(); }
            if (message.isEmpty()) message = changed ? "Stretch aplicado al pad " + juce::String(pad + 1) + ". UNDO disponible; ratio 1 restaura el original." : "El pad cambio durante el procesamiento; resultado descartado.";
            juce::MessageManager::callAsync([safe, message] {
                if (!safe) return; safe->processingSample = false; safe->refreshSelection(); safe->status.setText(message, juce::dontSendNotification);
            });
        });
    };
    addAndMakeVisible(chopCount); chopCount.setWantsKeyboardFocus(false);
    chopCount.addItem("4 slices", 4); chopCount.addItem("8 slices", 8); chopCount.addItem("16 slices", 16); chopCount.setSelectedId(16);
    addAndMakeVisible(chopButton); chopButton.setWantsKeyboardFocus(false);
    chopButton.setTooltip("Divide IN/OUT en partes iguales. Reemplaza pads 1-N. Una operacion UNDO restaura todos los destinos.");
    chopCount.onChange = [this] { waveform.setChopPreview(chopCount.getSelectedId()); chopButton.setButtonText("CHOP -> PADS 1-" + juce::String(chopCount.getSelectedId())); refreshSelection(); };
    waveform.setChopPreview(16);
    chopButton.onClick = [this] {
        const int pad = selected, count = chopCount.getSelectedId(); const auto versions = engine.getRevisions();
        processingSample = true; refreshSelection();
        status.setText("Dividiendo IN/OUT hacia pads 1-" + juce::String(count) + ".", juce::dontSendNotification);
        fileJobs.addJob([this, safe = juce::Component::SafePointer<MainComponent>(this), pad, count, versions] {
            const bool changed = !cancelProcessing.load() && engine.chopPad(pad, count, &versions);
            juce::MessageManager::callAsync([safe, changed, count] {
                if (!safe) return; safe->processingSample = false; safe->refreshSelection();
                safe->status.setText(changed ? "Chop listo en pads 1-" + juce::String(count) + ". Un UNDO restaura todos los destinos." : "Chop descartado: los pads cambiaron o la region es demasiado corta.", juce::dontSendNotification);
            });
        });
    };
    classicMode.setColour(juce::ToggleButton::textColourId, mint);
    classicMode.onClick = [this] {
        const int pad = selected; const bool enabled = classicMode.getToggleState();
        convertingClassic = true; classicMode.setEnabled(false);
        status.setText(enabled ? "Convirtiendo a 12 bits / 26.04 kHz en segundo plano..." : "Restaurando sonido original...", juce::dontSendNotification);
        fileJobs.addJob([this, safe = juce::Component::SafePointer<MainComponent>(this), pad, enabled] {
            bool changed = false;
            try { changed = engine.setClassicMode(pad, enabled, [this] { return cancelProcessing.load(); }); }
            catch (const std::exception&) { }
            juce::MessageManager::callAsync([safe, pad, enabled, changed] {
                if (!safe) return; safe->convertingClassic = false; safe->refreshSelection();
                safe->status.setText(changed ? "Pad " + juce::String(pad + 1) + (enabled ? ": 12 bits / 26.04 kHz. UNDO disponible." : ": sonido original restaurado.") : "El sample cambio durante la conversion. Vuelve a intentarlo.", juce::dontSendNotification);
            });
        });
    };
    addAndMakeVisible(inputRecordButton); inputRecordButton.setWantsKeyboardFocus(false);
    addAndMakeVisible(inputChannel); inputChannel.setWantsKeyboardFocus(false);
    inputChannel.addItem("INPUT 1 / MONO", 1); inputChannel.addItem("INPUT 2 / MONO", 2); inputChannel.addItem("INPUT 1 + 2 / STEREO", 3); inputChannel.setSelectedId(1);
    inputRecordButton.onClick = [this] {
        if (engine.hasCapture()) { engine.requestStopResampling(); return; }
        const int channel = inputChannel.getSelectedId() == 3 ? -1 : inputChannel.getSelectedId() - 1;
        int available = processor.getTotalNumInputChannels();
        if (auto* holder = juce::StandalonePluginHolder::getInstance()) {
            if (auto* device = holder->deviceManager.getCurrentAudioDevice()) available = device->getActiveInputChannels().countNumberOfSetBits();
            else available = 0;
            holder->getMuteInputValue().setValue(false);
        }
        if (available <= 0 || (channel >= 0 && channel >= available) || (channel < 0 && available < 2)) {
            status.setText("Activa los canales de entrada en AUDIO / MIDI (o en el DAW).", juce::dontSendNotification); return;
        }
        armed = false;
        status.setText(engine.startResampling(selected, true, channel) ? "Grabando entrada al pad " + juce::String(selected + 1) + ". STOP REC para terminar (max. 60 s)." : "Espera a que termine la captura anterior.", juce::dontSendNotification);
    };
    for (auto* slider : {&cutoff, &drive, &delay, &volume, &pitch, &pan, &trimStart, &trimEnd, &fadeIn, &fadeOut}) {
        addAndMakeVisible(slider); slider->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 88, 22); slider->setWantsKeyboardFocus(false);
        slider->setSliderStyle(slider == &trimStart || slider == &trimEnd || slider == &fadeIn || slider == &fadeOut ? juce::Slider::LinearHorizontal : juce::Slider::RotaryHorizontalVerticalDrag);
    }
    cutoff.setRange(40, 20000, 1); cutoff.setSkewFactorFromMidPoint(2000); cutoff.setTextValueSuffix(" Hz");
    drive.setRange(0, 1, 0.01); delay.setRange(0, 1, 0.01); volume.setRange(0, 2, 0.01);
    pitch.setRange(-24, 24, 0.01); pitch.setTextValueSuffix(" st"); pan.setRange(-1, 1, 0.01);
    trimStart.setRange(0, 100, 0.01); trimEnd.setRange(0, 100, 0.01); trimStart.setTextValueSuffix(" %"); trimEnd.setTextValueSuffix(" %");
    fadeIn.setRange(0, 100, 0.1); fadeOut.setRange(0, 100, 0.1); fadeIn.setTextValueSuffix(" ms"); fadeOut.setTextValueSuffix(" ms");
    cutoff.onValueChange = [this] { engine.cutoff = static_cast<float>(cutoff.getValue()); };
    drive.onValueChange = [this] { engine.drive = static_cast<float>(drive.getValue()); };
    delay.onValueChange = [this] { engine.delayMix = static_cast<float>(delay.getValue()); };
    for (auto* slider : {&volume, &pitch, &pan, &trimStart, &trimEnd, &fadeIn, &fadeOut}) {
      slider->onDragStart = [this] { engine.rememberPadEdit(selected); editingControls = true; };
      slider->onDragEnd = [this] { editingControls = false; refreshSelection(); };
      slider->onValueChange = [this, slider] {
        if (updating) return;
        if (!editingControls) engine.rememberPadEdit(selected);
        engine.editPad(selected, [this, slider](auto& data) {
            if (slider == &volume) data.volume = static_cast<float>(volume.getValue());
            if (slider == &pitch) data.pitchSemitones = static_cast<float>(pitch.getValue());
            if (slider == &pan) data.pan = static_cast<float>(pan.getValue());
            if (slider == &fadeIn) data.fadeInMs = static_cast<float>(fadeIn.getValue());
            if (slider == &fadeOut) data.fadeOutMs = static_cast<float>(fadeOut.getValue());
            const int size = data.audioData.getNumSamples();
            if (slider == &trimStart || slider == &trimEnd) {
                const auto sample = static_cast<int64_t>(size * (slider == &trimStart ? trimStart.getValue() : trimEnd.getValue()) / 100);
                WaveformViewport::moveMarker(slider == &trimStart, sample, size, data.startSample, data.endSample);
                data.loopStartSample = data.startSample; data.loopEndSample = data.endSample;
            }
        });
        waveform.setPad(engine.getPad(selected));
        if (!editingControls) refreshSelection();
        repaint();
      };
    }
    mode.addItem("ONE SHOT", 1); mode.addItem("GATE", 2); mode.addItem("LOOP", 3);
    choke.addItem("Choke: off", 1); for (int i = 1; i <= 8; ++i) choke.addItem("Choke: " + juce::String(i), i + 1);
    for (auto* combo : {&mode, &choke}) { addAndMakeVisible(combo); combo->setWantsKeyboardFocus(false); }
    mode.onChange = [this] { engine.rememberPadEdit(selected); engine.editPad(selected, [this](auto& data) { data.mode = static_cast<model::PlaybackMode>(mode.getSelectedId() - 1); }); };
    choke.onChange = [this] { engine.rememberPadEdit(selected); engine.editPad(selected, [this](auto& data) { data.chokeGroup = choke.getSelectedId() - 1; }); };
    addAndMakeVisible(waveform);
    waveform.onBeginEdit = [this] { engine.rememberPadEdit(selected); };
    waveform.onTrim = [this](int64_t start, int64_t end) {
        engine.editPad(selected, [start, end](auto& data) { data.startSample = data.loopStartSample = start; data.endSample = data.loopEndSample = end; });
        refreshSelection();
    };
    addAndMakeVisible(status); status.setColour(juce::Label::textColourId, muted);
    status.setText("Carga audio o arrastra un archivo sobre un pad. MIDI: notas 36-51.", juce::dontSendNotification);
    refreshSelection();
    setResizable(true, false);
    setResizeLimits(designWidth / 2, designHeight / 2, designWidth * 2, designHeight * 2);
    getConstrainer()->setFixedAspectRatio(static_cast<double>(designWidth) / designHeight);
    // Embedded editors share the screen with the DAW's browser, toolbars and wrapper.
    float initialScale = juce::JUCEApplicationBase::isStandaloneApp() ? 1.0f : 0.7f;
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        initialScale = juce::jlimit(0.5f, initialScale, std::min(
            static_cast<float>(display->userArea.getWidth() - 80) / designWidth,
            static_cast<float>(display->userArea.getHeight() - 100) / designHeight));
    setSize(juce::roundToInt(designWidth * initialScale), juce::roundToInt(designHeight * initialScale));
    startTimerHz(25);
}
MainComponent::~MainComponent() { stopTimer(); cancelProcessing = true; ++creativeGeneration; ++previewRequest; engine.stopPreview(); fileJobs.removeAllJobs(true, -1); rescueJobs.removeAllJobs(true, -1); engine.panic(); setLookAndFeel(nullptr); }
void MainComponent::refreshSelection() {
    updating = true; const auto data = engine.getPad(selected);
    classicMode.setToggleState(data.classic12Bit, juce::dontSendNotification);
    classicMode.setEnabled(data.hasSample() && !convertingClassic && !processingSample);
    stretch.setValue(data.stretchRatio, juce::dontSendNotification);
    stretch.setEnabled(data.hasSample() && !processingSample && !convertingClassic);
    stretchButton.setEnabled(data.hasSample() && !processingSample && !convertingClassic);
    chopCount.setEnabled(!processingSample); chopButton.setEnabled(data.hasSample()
        && data.endSample - data.startSample >= chopCount.getSelectedId() && !processingSample && !convertingClassic);
    volume.setValue(data.volume, juce::dontSendNotification); pitch.setValue(data.pitchSemitones, juce::dontSendNotification); pan.setValue(data.pan, juce::dontSendNotification);
    const double size = std::max(1, data.audioData.getNumSamples());
    trimStart.setValue(100 * static_cast<double>(data.startSample) / size, juce::dontSendNotification);
    trimEnd.setValue(data.hasSample() ? 100 * static_cast<double>(data.endSample) / size : 100, juce::dontSendNotification);
    mode.setSelectedId(static_cast<int>(data.mode) + 1, juce::dontSendNotification); choke.setSelectedId(data.chokeGroup + 1, juce::dontSendNotification);
    fadeIn.setValue(data.fadeInMs, juce::dontSendNotification); fadeOut.setValue(data.fadeOutMs, juce::dontSendNotification); waveform.setPad(data);
    cutoff.setValue(engine.cutoff.load(), juce::dontSendNotification); drive.setValue(engine.drive.load(), juce::dontSendNotification); delay.setValue(engine.delayMix.load(), juce::dontSendNotification);
    updating = false; repaint(); for (auto& pad : pads) pad->repaint();
}
void MainComponent::press(int pad) {
    selected = pad; refreshSelection(); grabKeyboardFocus();
    if (armed) { armed = false; status.setText(engine.startResampling(pad) ? "Grabando al pad " + juce::String(pad + 1) + ". Toca otros pads; RESAMPLE para terminar (max. 60 s)." : "Espera a que termine el resample anterior.", juce::dontSendNotification); }
    else engine.enqueue(pad, true);
}
void MainComponent::release(int pad) { engine.enqueue(pad, false); }
void MainComponent::load(int pad, const juce::File& file) {
    status.setText("Cargando en segundo plano: " + file.getFileName(), juce::dontSendNotification);
    fileJobs.addJob([this, safe = juce::Component::SafePointer<MainComponent>(this), pad, file] {
        juce::String error; const bool success = engine.loadFile(pad, file, error);
        juce::MessageManager::callAsync([safe, pad, file, error, success] {
            if (!safe) return;
            if (success) { if (safe->selected == pad) safe->refreshSelection(); safe->status.setText("Sample cargado: " + file.getFileName(), juce::dontSendNotification); }
            else safe->status.setText(error, juce::dontSendNotification);
        });
    });
}
void MainComponent::toggleResample() {
    if (engine.hasCapture()) { engine.requestStopResampling(); status.setText("Finalizando captura...", juce::dontSendNotification); }
    else { armed = !armed; status.setText(armed ? "Selecciona el pad de destino para iniciar la captura." : "Resample cancelado.", juce::dontSendNotification); }
}
std::array<model::PadData, 16> MainComponent::currentPads() const {
    const juce::ScopedLock guard(engine.mutex); return engine.pads;
}
void MainComponent::closeCreativePanel() {
    ++creativeGeneration; ++previewRequest; engine.stopPreview(); creativePanel.dismiss();
}
void MainComponent::openVariations() {
    const auto source = engine.getPad(selected);
    if (!source.hasSample()) { status.setText("Carga un sample en el pad seleccionado primero.", juce::dontSendNotification); return; }
    closeCreativePanel();
    sequencerPanel.setVisible(false);
    creativePanel.showVariations(source, selected, currentPads()); creativePanel.toFront(true);
}
void MainComponent::rescueImprovisation() {
    sequencerPanel.setVisible(false);
    closeCreativePanel(); const auto request = creativeGeneration.load();
    const auto position = engine.markRecentAudio();
    status.setText("Recuperando los ultimos 30 segundos...", juce::dontSendNotification);
    rescueJobs.addJob([this, safe = juce::Component::SafePointer<MainComponent>(this), request, position] {
        auto data = engine.rescueRecentAudio(position);
        juce::MessageManager::callAsync([safe, request, data = std::move(data)]() mutable {
            if (!safe || safe->creativeGeneration.load() != request) return;
            if (!data.hasSample()) { safe->status.setText("No se pudo rescatar audio. Toca unos pads y vuelve a pulsar RESCATAR.", juce::dontSendNotification); return; }
            safe->creativePanel.showRescue(std::move(data), safe->currentPads()); safe->creativePanel.toFront(true);
            safe->status.setText("Improvisacion recuperada. Escucha, recorta y elige el destino.", juce::dontSendNotification);
        });
    });
}
void MainComponent::generateVariations(model::PadData data, audio::VariationOptions options) {
    const auto request = ++creativeGeneration;
    ++previewRequest; engine.stopPreview();
    const auto seed = juce::Random::getSystemRandom().nextInt64();
    creativePanel.setBusy(true); creativePanel.setMessage("Generando cuatro alternativas...");
    fileJobs.addJob([this, safe = juce::Component::SafePointer<MainComponent>(this), request, seed, options, data = std::move(data)] {
        std::array<model::PadData, 4> candidates; juce::String error;
        try { candidates = audio::makePadVariations(data, options, seed, [this, request] {
            return cancelProcessing.load() || creativeGeneration.load() != request;
        }); } catch (const std::exception& problem) { error = problem.what(); }
        juce::MessageManager::callAsync([safe, request, error, candidates = std::move(candidates)]() mutable {
            if (!safe || safe->creativeGeneration.load() != request) return;
            if (error.isNotEmpty()) { safe->creativePanel.setBusy(false); safe->creativePanel.setMessage(error); }
            else safe->creativePanel.setCandidates(std::move(candidates));
        });
    });
}
void MainComponent::saveCreativeDraft(model::PadData data, int destination, bool crop) {
    const auto request = creativeGeneration.load();
    uint64_t revision; bool occupied;
    { const juce::ScopedLock guard(engine.mutex);
      revision = engine.getRevisions()[destination]; occupied = engine.pads[destination].hasSample(); }
    creativePanel.setBusy(true);
    auto save = [safe = juce::Component::SafePointer<MainComponent>(this), request, revision, occupied, destination, crop, data = std::move(data)](int result) mutable {
        if (!safe || safe->creativeGeneration.load() != request) return;
        if (result != 1) { safe->creativePanel.setBusy(false); return; }
        safe->creativePanel.setMessage("Preparando audio para guardar...");
        auto* editor = safe.getComponent();
        editor->fileJobs.addJob([safe, request, revision, occupied, destination, crop, data = std::move(data)]() mutable {
            juce::String error;
            try { if (crop) data = audio::trimRescuedAudio(std::move(data)); }
            catch (const std::exception& problem) { error = problem.what(); }
            juce::MessageManager::callAsync([safe, request, revision, occupied, destination, error, data = std::move(data)]() mutable {
                if (!safe || safe->creativeGeneration.load() != request) return;
                if (error.isNotEmpty()) { safe->creativePanel.setBusy(false); safe->creativePanel.setMessage(error); return; }
                if (!safe->engine.commitDraft(destination, std::move(data), revision, occupied)) {
                    safe->creativePanel.setBusy(false); safe->creativePanel.setMessage("El destino cambio. Vuelve a elegir un pad y guarda otra vez."); return;
                }
                safe->selected = destination; safe->closeCreativePanel(); safe->refreshSelection();
                safe->status.setText("Guardado en pad " + juce::String(destination + 1) + ". UNDO disponible.", juce::dontSendNotification);
            });
        });
    };
    if (occupied) juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::WarningIcon, "Reemplazar pad",
        "El pad " + juce::String(destination + 1) + " contiene un sample. Reemplazarlo? UNDO permite recuperarlo.",
        "Reemplazar", "Cancelar", this, juce::ModalCallbackFunction::create(save));
    else save(1);
}
void MainComponent::requestNewProject() {
    newProjectButton.setEnabled(false);
    juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::WarningIcon, "Nuevo proyecto",
        "Se vaciaran los 16 pads, los patrones, los efectos y el historial. Los cambios sin guardar se perderan.\n\n"
        "Si quieres conservar el trabajo actual, cancela y pulsa SAVE primero.",
        "Empezar de cero", "Cancelar", this,
        juce::ModalCallbackFunction::create([safe = juce::Component::SafePointer<MainComponent>(this)](int result) {
            if (!safe) return;
            safe->newProjectButton.setEnabled(true);
            if (result != 1) return;
            safe->closeCreativePanel();
            safe->sequencerPanel.setVisible(false);
            ++safe->projectGeneration; ++safe->previewRequest; safe->chooser.reset();
            safe->resettingProject = true; safe->cancelProcessing = true;
            safe->engine.stopPreview(); safe->engine.cancelResampling(); safe->engine.panic();
            safe->armed = false; safe->held.fill(false); safe->setEnabled(false);
            safe->status.setText("Preparando proyecto nuevo...", juce::dontSendNotification);
            // Run after earlier file jobs so a late load/open cannot repopulate the new project.
            auto* editor = safe.getComponent();
            editor->fileJobs.addJob([editor, safe] {
                editor->engine.newProject();
                juce::MessageManager::callAsync([safe] {
                    if (!safe) return;
                    safe->resettingProject = false; safe->cancelProcessing = false;
                    safe->processingSample = safe->convertingClassic = safe->finishingCapture = false;
                    safe->editingControls = false; safe->selected = 0;
                    safe->inputChannel.setSelectedId(1, juce::dontSendNotification);
                    safe->chopCount.setSelectedId(16, juce::dontSendNotification);
                    safe->chopButton.setButtonText("CHOP -> PADS 1-16"); safe->waveform.setChopPreview(16);
                    safe->setEnabled(true); safe->refreshSelection();
                    safe->status.setText("Proyecto nuevo. Carga un sample para empezar.", juce::dontSendNotification);
                    safe->grabKeyboardFocus();
                });
            });
        }));
}
void MainComponent::chooseFile(int kind) {
    if (kind == 2) closeCreativePanel();
    chooser = std::make_unique<juce::FileChooser>(kind == 0 ? "Cargar sample" : "Proyecto StudioSP", juce::File{}, kind == 0 ? "*.wav;*.flac;*.aiff;*.aif" : "*.studiosp");
    const int destination = selected;
    const int dialogFlags = kind == 1 ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting : juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
    chooser->launchAsync(dialogFlags, [safe = juce::Component::SafePointer<MainComponent>(this), kind, destination, generation = projectGeneration](const juce::FileChooser& dialog) {
        if (!safe || safe->projectGeneration != generation) return; auto file = dialog.getResult(); if (file == juce::File{}) return;
        if (kind == 0) { safe->load(destination, file); return; }
        safe->status.setText(kind == 1 ? "Guardando proyecto en segundo plano..." : "Abriendo proyecto en segundo plano...", juce::dontSendNotification);
        auto* editor = safe.getComponent();
        editor->fileJobs.addJob([editor, safe, kind, file]() mutable {
            juce::MemoryBlock block; juce::String message; bool refresh = false;
            if (kind == 1) {
                editor->processor.getStateInformation(block); file = file.withFileExtension("studiosp");
                message = file.replaceWithData(block.getData(), block.getSize()) ? "Proyecto guardado: " + file.getFileName() : "No se pudo guardar el proyecto.";
            } else {
                if (!file.loadFileAsData(block) || block.getSize() > static_cast<size_t>(std::numeric_limits<int>::max())) message = "No se pudo abrir el proyecto.";
                else { refresh = editor->processor.restoreState(block.getData(), static_cast<int>(block.getSize())); message = refresh ? "Proyecto abierto: " + file.getFileName() : "Proyecto invalido o incompleto."; }
            }
            juce::MessageManager::callAsync([safe, message, refresh] { if (safe) { if (refresh) safe->refreshSelection(); safe->status.setText(message, juce::dontSendNotification); } });
        });
    });
}
void MainComponent::exportPatternMidi(audio::PatternSequencer::State state, int pattern, double tempo) {
    const auto letter = juce::String::charToString(static_cast<juce::juce_wchar>('A' + pattern));
    chooser = std::make_unique<juce::FileChooser>("Exportar patron MIDI " + letter,
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("StudioSP-Pattern-" + letter + ".mid"), "*.mid");
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
        | juce::FileBrowserComponent::warnAboutOverwriting,
        [safe = juce::Component::SafePointer<MainComponent>(this), state = std::move(state), pattern, tempo](const juce::FileChooser& dialog) {
            if (!safe) return;
            auto file = dialog.getResult(); if (file == juce::File{}) return;
            if (!file.hasFileExtension("mid")) {
                safe->status.setText("Elige un nombre terminado en .mid para exportar MIDI.", juce::dontSendNotification); return;
            }
            safe->status.setText("Exportando patron MIDI...", juce::dontSendNotification);
            safe->fileJobs.addJob([safe, file, state, pattern, tempo] {
                juce::MemoryOutputStream bytes;
                const bool saved = audio::makePatternMidi(state, pattern, tempo).writeTo(bytes, 0)
                    && file.replaceWithData(bytes.getData(), bytes.getDataSize());
                juce::MessageManager::callAsync([safe, saved, file] {
                    if (safe) safe->status.setText(saved ? "MIDI exportado: " + file.getFileName() : "No se pudo exportar el archivo MIDI.", juce::dontSendNotification);
                });
            });
        });
}
bool MainComponent::keyPressed(const juce::KeyPress& key) {
    if (dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent()) != nullptr) return false;
    if (key.getKeyCode() == juce::KeyPress::spaceKey) { ++previewRequest; engine.stopPreview(); engine.panic(); return true; }
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'Z') { engine.history(key.getModifiers().isShiftDown()); refreshSelection(); return true; }
    return keyStateChanged(true);
}
bool MainComponent::keyStateChanged(bool) {
    if (dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent()) != nullptr) return false;
    bool handled = false;
    for (int i = 0; i < 16; ++i) {
        const bool down = juce::KeyPress::isKeyCurrentlyDown(keys[i]);
        if (down != held[i]) { held[i] = down; if (down) press(i); else release(i); handled = true; }
    }
    return handled;
}
void MainComponent::focusLost(FocusChangeType) { for (int i = 0; i < 16; ++i) if (held[i]) { held[i] = false; release(i); } }
void MainComponent::timerCallback() {
    if (resettingProject) return;
    if (!inputEnabled) if (auto* holder = juce::StandalonePluginHolder::getInstance()) {
        holder->getMuteInputValue().setValue(false); inputEnabled = true;
    }
    engine.collectRetired();
    if (engine.hasCapture() && engine.isCaptureReady() && !finishingCapture) {
        finishingCapture = true;
        fileJobs.addJob([this, safe = juce::Component::SafePointer<MainComponent>(this)] {
            const bool saved = engine.finishResampling();
            juce::MessageManager::callAsync([safe, saved] {
                if (!safe) return; safe->finishingCapture = false; safe->refreshSelection();
                safe->status.setText(saved ? "Captura guardada en el pad. UNDO recupera el sample anterior." : "Captura vacia o cancelada.", juce::dontSendNotification);
            });
        });
    }
    resampleButton.setButtonText(engine.isCaptureStopping() || finishingCapture ? "FINALIZANDO" : engine.recording.load() ? "FINISH REC" : armed ? "SELECT PAD" : "RESAMPLE");
    inputRecordButton.setButtonText(engine.isCaptureStopping() || finishingCapture ? "FINALIZANDO" : engine.hasCapture() && engine.isInputCapture() ? "STOP REC" : "REC INPUT");
    inputRecordButton.setEnabled(!engine.hasCapture() || engine.isInputCapture());
    inputChannel.setEnabled(!engine.hasCapture());
    repaint(); for (auto& pad : pads) pad->repaint();
}
void MainComponent::resized() {
    sequencerButton.setBounds(782, 70, 120, 30);
    sequencerPanel.setBounds(32, 174, 1048, 700);
    stretch.setBounds(32, 70, 230, 30); stretchButton.setBounds(274, 70, 140, 30);
    chopCount.setBounds(438, 70, 110, 30); chopButton.setBounds(560, 70, 210, 30);
    classicMode.setBounds(846, 182, 220, 26);
    sampleBrowser.setBounds(1100, 28, 340, 846);
    inputChannel.setBounds(32, 766, 270, 32); inputRecordButton.setBounds(318, 766, 170, 32);
    newProjectButton.setBounds(550, 28, 122, 32);
    variationsButton.setBounds(636, 112, 186, 32); rescueButton.setBounds(834, 112, 234, 32);
    creativePanel.setBounds(32, 174, 1048, 700);
    interfaceSize.setBounds(914, 70, 154, 30);
    openButton.setBounds(680, 28, 70, 32); saveButton.setBounds(758, 28, 70, 32); audioButton.setBounds(840, 28, 228, 32);
    loadButton.setBounds(32, 112, 140, 34); resampleButton.setBounds(182, 112, 144, 34); stopButton.setBounds(336, 112, 90, 34);
    undoButton.setBounds(438, 112, 72, 34); redoButton.setBounds(518, 112, 72, 34);
    for (int i = 0; i < 16; ++i) { const int row = 3 - i / 4, col = i % 4; pads[i]->setBounds(32 + col * 141, 190 + row * 128, 129, 116); }
    cutoff.setBounds(636, 550, 135, 104); drive.setBounds(786, 550, 135, 104); delay.setBounds(936, 550, 135, 104);
    mode.setBounds(636, 666, 138, 32); choke.setBounds(786, 666, 138, 32); clearButton.setBounds(936, 666, 130, 32);
    volume.setBounds(636, 730, 135, 106); pitch.setBounds(786, 730, 135, 106); pan.setBounds(936, 730, 135, 106);
    waveform.setBounds(636, 254, 430, 128);
    trimStart.setBounds(636, 385, 205, 50); trimEnd.setBounds(861, 385, 205, 50);
    fadeIn.setBounds(636, 451, 205, 45); fadeOut.setBounds(861, 451, 205, 45);
    status.setBounds(32, 886, 1036, 26);
    interfaceScale = std::min(static_cast<float>(getWidth()) / designWidth,
                              static_cast<float>(getHeight()) / designHeight);
    const int percent = juce::roundToInt(interfaceScale * 100);
    interfaceSize.setSelectedId(0, juce::dontSendNotification);
    interfaceSize.setText("UI " + juce::String(percent) + "%", juce::dontSendNotification);
    for (auto* child : getChildren()) child->setTransform(juce::AffineTransform::scale(interfaceScale));
}
void MainComponent::paint(juce::Graphics& g) {
    g.fillAll(background);
    g.addTransform(juce::AffineTransform::scale(interfaceScale));
    g.drawImage(brandLogo, juce::Rectangle<float>{32, 22, 280, 45}, juce::RectanglePlacement::centred);
    text(g, "TACTILE SAMPLER / 32-BIT FLOAT", {328, 28, 208, 32}, 10, muted);
    g.setColour(panel); g.fillRoundedRectangle(620, 174, 460, 326, 10);
    text(g, "SAMPLE / PAD " + juce::String(selected + 1).paddedLeft('0', 2), {636, 184, 205, 24}, 12, mint);
    { const juce::ScopedLock guard(engine.mutex); const auto& pad = engine.pads[selected];
      text(g, juce::String(pad.sampleName), {636, 209, 350, 28}, 19, juce::Colours::white);
      text(g, (pad.classic12Bit ? "12-bit / 26.04 kHz / " : juce::String(pad.sampleRate / 1000, 1) + " kHz / ") + juce::String(pad.audioData.getNumSamples() / pad.sampleRate * pad.stretchRatio, 2) + " s / SOURCE WAVE", {636, 238, 430, 22}, 12, muted); }
    text(g, "FADE IN", {636, 435, 205, 16}, 10, muted);
    text(g, "FADE OUT", {861, 435, 205, 16}, 10, muted);
    text(g, "FX / MASTER BUS", {636, 508, 400, 24}, 12, mint);
    text(g, "CUTOFF", {636, 534, 135, 20}, 11, muted, juce::Justification::centred);
    text(g, "DRIVE", {786, 534, 135, 20}, 11, muted, juce::Justification::centred);
    text(g, "DELAY", {936, 534, 135, 20}, 11, muted, juce::Justification::centred);
    text(g, "LEVEL", {636, 712, 135, 20}, 11, muted, juce::Justification::centred);
    text(g, "PITCH", {786, 712, 135, 20}, 11, muted, juce::Justification::centred);
    text(g, "PAN", {936, 712, 135, 20}, 11, muted, juce::Justification::centred);
    text(g, "16 PADS / BANK A", {32, 158, 400, 25}, 12, muted);
    text(g, "SPACE: STOP ALL     /     DRAG AUDIO TO A PAD", {32, 711, 580, 24}, 11, muted);
    text(g, "WAVEFORM: WHEEL TO ZOOM / SHIFT + WHEEL TO PAN", {32, 746, 570, 24}, 11, muted);
    text(g, "INPUT LEVEL / DRY RECORDING TO SELECTED PAD", {32, 808, 570, 24}, 11, muted);
    g.setColour(panel); g.fillRoundedRectangle(32, 842, 556, 7, 3);
    g.setColour(engine.inputPeak.load() >= 1 ? orange : mint); g.fillRoundedRectangle(32, 842, 556 * juce::jlimit(0.0f, 1.0f, engine.inputPeak.load()), 7, 3);
    g.setColour(panel); g.fillRoundedRectangle(636, 846, 430, 7, 3);
    g.setColour(engine.recording.load() ? orange : mint); g.fillRoundedRectangle(636, 846, 430 * engine.peak.load(), 7, 3);
    if (engine.recording.load()) text(g, "REC " + juce::String(engine.recordedSamples.load() / engine.getSampleRate(), 1) + " s", {636, 860, 430, 22}, 12, orange);
}

void PadComponent::paint(juce::Graphics& g) {
    const bool active = (editor.engine.activePads.load() & (1u << pad)) != 0, selected = editor.selected == pad;
    const auto bounds = getLocalBounds().toFloat().reduced(1);
    g.setColour(active ? mint.withAlpha(0.25f) : panel); g.fillRoundedRectangle(bounds, 9);
    g.setColour(hovering || selected || active ? mint : juce::Colour(0xff323b3e)); g.drawRoundedRectangle(bounds, 9, selected ? 2.0f : 1.0f);
    text(g, juce::String(pad + 1).paddedLeft('0', 2), {12, 8, 64, 28}, 21, active ? mint : juce::Colours::white);
    text(g, juce::String::charToString(MainComponent::keys[pad]), {86, 12, 28, 22}, 12, muted, juce::Justification::centred);
    const juce::ScopedLock guard(editor.engine.mutex); const auto& data = editor.engine.pads[pad];
    text(g, data.hasSample() ? juce::String(data.sampleName) : "Empty pad", {12, 66, 104, 28}, 12, data.hasSample() ? juce::Colours::white : muted);
    g.setColour(data.hasSample() ? mint : muted.withAlpha(0.3f)); g.fillRoundedRectangle(12, 100, data.hasSample() ? 104.0f : 22.0f, 3, 1);
}
void PadComponent::mouseDown(const juce::MouseEvent& event) { if (event.mods.isRightButtonDown()) return; editor.press(pad); }
void PadComponent::mouseUp(const juce::MouseEvent&) { editor.release(pad); }
bool PadComponent::isInterestedInFileDrag(const juce::StringArray& files) { return files.size() == 1 && juce::File(files[0]).hasFileExtension("wav;flac;aiff;aif"); }
void PadComponent::filesDropped(const juce::StringArray& files, int, int) { hovering = false; if (files.size() == 1) editor.load(pad, juce::File(files[0])); repaint(); }
}
