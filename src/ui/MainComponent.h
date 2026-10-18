#pragma once
#include "PluginProcessor.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include "WaveformComponent.h"
#include "SampleBrowser.h"
#include "CreativePanel.h"
#include "SequencerPanel.h"
namespace studiosp::ui {
class MainComponent;
class PadComponent : public juce::Component, public juce::FileDragAndDropTarget {
public:
    PadComponent(MainComponent& owner, int index) : editor(owner), pad(index) {}
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    bool isInterestedInFileDrag(const juce::StringArray&) override;
    void filesDropped(const juce::StringArray&, int, int) override;
    void fileDragEnter(const juce::StringArray&, int, int) override { hovering = true; repaint(); }
    void fileDragExit(const juce::StringArray&) override { hovering = false; repaint(); }
private:
    MainComponent& editor;
    int pad;
    bool hovering = false;
};
class MainComponent : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit MainComponent(StudioSPProcessor&);
    ~MainComponent() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    bool keyStateChanged(bool) override;
    void focusLost(FocusChangeType) override;
    void press(int pad);
    void release(int pad);
    void load(int pad, const juce::File&);
    studiosp::audio::AudioEngine& engine;
    int selected = 0;
    static constexpr const char* keys = "ZXCVASDFQWER1234";
private:
    StudioSPProcessor& processor;
    juce::LookAndFeel_V4 look;
    juce::Image brandLogo;
    std::array<std::unique_ptr<PadComponent>, 16> pads;
    std::array<bool, 16> held{};
    juce::TextButton loadButton{"LOAD SAMPLE"}, saveButton{"SAVE"}, openButton{"OPEN"}, resampleButton{"RESAMPLE"}, stopButton{"STOP"}, undoButton{"UNDO"}, redoButton{"REDO"}, clearButton{"CLEAR"}, audioButton{"AUDIO / MIDI"};
    juce::TextButton newProjectButton{"NEW PROJECT"};
    juce::TextButton variationsButton{"VARIACIONES"}, rescueButton{"RESCATAR / 30 s"};
    CreativePanel creativePanel;
    SequencerPanel sequencerPanel{engine};
    juce::TextButton sequencerButton{"SEQUENCER"};
    std::atomic<uint64_t> creativeGeneration{0};
    juce::Slider cutoff, drive, delay, volume, pitch, pan, trimStart, trimEnd, fadeIn, fadeOut;
    WaveformComponent waveform;
    SampleBrowser sampleBrowser;
    juce::TextButton inputRecordButton{"REC INPUT"};
    juce::ComboBox inputChannel;
    juce::ComboBox interfaceSize;
    juce::ToggleButton classicMode{"12 BIT / 26.04 kHz"};
    juce::Slider stretch;
    juce::TextButton stretchButton{"APPLY STRETCH"}, chopButton{"CHOP -> PADS 1-16"};
    juce::ComboBox chopCount;
    std::atomic<bool> cancelProcessing{false};
    bool processingSample = false;
    uint64_t previewRequest = 0;
    uint64_t projectGeneration = 0;
    bool resettingProject = false;
    bool convertingClassic = false;
    juce::ThreadPool fileJobs{1};
    juce::ThreadPool rescueJobs{1};
    juce::ComboBox mode, choke;
    juce::Label status;
    std::unique_ptr<juce::FileChooser> chooser;
    bool armed = false, updating = false, editingControls = false, finishingCapture = false, inputEnabled = false;
    static constexpr int designWidth = 1460, designHeight = 930;
    float interfaceScale = 1.0f;
    void timerCallback() override;
    void refreshSelection();
    void chooseFile(int kind);
    void toggleResample();
    void requestNewProject();
    void closeCreativePanel();
    void openVariations();
    void rescueImprovisation();
    void generateVariations(model::PadData, audio::VariationOptions);
    void saveCreativeDraft(model::PadData, int destination, bool crop);
    std::array<model::PadData, 16> currentPads() const;
};
}
