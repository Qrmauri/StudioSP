#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <vector>
#include "model/SampleLibrary.h"
#include <atomic>
#include "model/SampleFolderSettings.h"

namespace studiosp::ui {
class SampleBrowser : public juce::Component, private juce::ListBoxModel {
public:
    std::function<void(const juce::File&)> onLoad;
    std::function<void(const juce::File&)> onPreview;
    std::function<void()> onStopPreview;
    std::function<void(const juce::String&)> onError;
    SampleBrowser() {
        for (auto* component : std::initializer_list<juce::Component*>{&folder, &search, &list, &load, &info, &preview, &stopPreview}) addAndMakeVisible(component);
        folder.setButtonText("ADD SAMPLE FOLDER"); load.setButtonText("LOAD INTO SELECTED PAD");
        folder.setWantsKeyboardFocus(false); load.setWantsKeyboardFocus(false);
        search.setTextToShowWhenEmpty("Search name or folder...", juce::Colours::grey);
        search.onTextChange = [this] { filter(); };
        list.setModel(this); list.setRowHeight(48);
        list.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff111416));
        load.onClick = [this] { loadSelected(); }; load.setEnabled(false);
        preview.setButtonText("PREVIEW"); stopPreview.setButtonText("STOP PREVIEW");
        preview.setWantsKeyboardFocus(false); stopPreview.setWantsKeyboardFocus(false); preview.setEnabled(false);
        preview.onClick = [this] {
            const int row = list.getSelectedRow();
            if (onPreview && row >= 0 && row < getNumRows()) onPreview(visible[static_cast<size_t>(row)]);
        };
        stopPreview.onClick = [this] { if (onStopPreview) onStopPreview(); };
        info.setText("Add folders containing WAV, FLAC or AIFF.", juce::dontSendNotification);
        folder.onClick = [this] {
            chooser = std::make_unique<juce::FileChooser>("Carpeta de samples", juce::File{}, "");
            chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                [safe = juce::Component::SafePointer<SampleBrowser>(this)](const juce::FileChooser& dialog) {
                    if (!safe || !dialog.getResult().isDirectory()) return;
                    const auto root = dialog.getResult();
                    if (std::find(safe->roots.begin(), safe->roots.end(), root) == safe->roots.end()) safe->roots.push_back(root);
                    if (!model::writeSampleFolders(model::sampleFolderSettingsFile(), safe->roots) && safe->onError)
                        safe->onError("No se pudieron guardar las carpetas del buscador.");
                    safe->scan();
                });
        };
        roots = model::readSampleFolders(model::sampleFolderSettingsFile());
        if (!roots.empty()) scan();
    }
    ~SampleBrowser() override { scanCancelled->store(true); list.setModel(nullptr); }
    void resized() override {
        folder.setBounds(12, 42, getWidth() - 24, 32);
        search.setBounds(12, 86, getWidth() - 24, 32);
        info.setBounds(12, 123, getWidth() - 24, 36);
        list.setBounds(12, 164, getWidth() - 24, getHeight() - 264);
        preview.setBounds(12, getHeight() - 88, (getWidth() - 32) / 2, 32);
        stopPreview.setBounds(20 + (getWidth() - 32) / 2, getHeight() - 88, (getWidth() - 32) / 2, 32);
        load.setBounds(12, getHeight() - 44, getWidth() - 24, 32);
    }
    void paint(juce::Graphics& g) override {
        g.setColour(juce::Colour(0xff1c2225)); g.fillRoundedRectangle(getLocalBounds().toFloat(), 10);
        g.setColour(juce::Colour(0xffbcf468)); g.setFont(14); g.drawText("LOCAL SAMPLES", 12, 8, getWidth() - 24, 28, juce::Justification::centredLeft);
    }
private:
    juce::TextButton folder, load, preview, stopPreview;
    juce::TextEditor search;
    juce::Label info;
    juce::ListBox list;
    std::shared_ptr<std::atomic<bool>> scanCancelled = std::make_shared<std::atomic<bool>>(false);
    std::unique_ptr<juce::FileChooser> chooser;
    std::vector<juce::File> roots, files, visible;
    static juce::ThreadPool& scanJobs() {
        // Jobs own their data and cancellation flag; closing an editor never waits on disk I/O.
        static juce::ThreadPool pool{1};
        return pool;
    }
    int getNumRows() override { return static_cast<int>(visible.size()); }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override {
        if (row < 0 || row >= getNumRows()) return;
        if (selected) { g.setColour(juce::Colour(0xff34452a)); g.fillAll(); }
        const auto& file = visible[static_cast<size_t>(row)];
        g.setColour(juce::Colours::white); g.setFont(14); g.drawText(file.getFileName(), 6, 2, width - 12, 22, juce::Justification::centredLeft);
        g.setColour(juce::Colours::grey); g.setFont(11); g.drawText(file.getParentDirectory().getFullPathName(), 6, 24, width - 12, height - 24, juce::Justification::centredLeft);
    }
    void selectedRowsChanged(int) override {
        load.setEnabled(list.getSelectedRow() >= 0); preview.setEnabled(list.getSelectedRow() >= 0);
        if (onStopPreview) onStopPreview();
    }
    void listBoxItemDoubleClicked(int, const juce::MouseEvent&) override { loadSelected(); }
    void returnKeyPressed(int) override { loadSelected(); }
    void loadSelected() {
        const int row = list.getSelectedRow();
        if (onLoad && row >= 0 && row < getNumRows()) {
            if (onStopPreview) onStopPreview(); onLoad(visible[static_cast<size_t>(row)]);
        }
    }
    void filter() {
        visible = model::searchSamples(files, search.getText());
        list.deselectAllRows(); list.updateContent(); list.repaint(); load.setEnabled(false);
        info.setText(juce::String(getNumRows()) + " samples / " + juce::String(static_cast<int>(roots.size())) + " folders", juce::dontSendNotification);
    }
    void scan() {
        folder.setEnabled(false); info.setText("Scanning subfolders...", juce::dontSendNotification);
        const auto cancelled = scanCancelled;
        scanJobs().addJob([safe = juce::Component::SafePointer<SampleBrowser>(this), folders = roots, cancelled] {
            auto found = model::scanSampleFolders(folders, [cancelled] { return cancelled->load(); });
            if (cancelled->load()) return;
            juce::MessageManager::callAsync([safe, cancelled, found = std::move(found)]() mutable {
                if (cancelled->load() || !safe) return; safe->files = std::move(found); safe->folder.setEnabled(true); safe->filter();
            });
        });
    }
};
}
