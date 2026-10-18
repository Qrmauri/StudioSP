#pragma once
#include "model/PadData.h"
#include "WaveformViewport.h"
#include <juce_gui_basics/juce_gui_basics.h>
namespace studiosp::ui {
class WaveformComponent : public juce::Component {
public:
    WaveformComponent() {
        for (auto* button : {&zoomIn, &zoomOut, &fit, &selection}) { addAndMakeVisible(button); button->setWantsKeyboardFocus(false); }
        zoomIn.onClick = [this] { view.zoom(2, 0.5); repaint(); }; zoomOut.onClick = [this] { view.zoom(0.5, 0.5); repaint(); };
        fit.onClick = [this] { view.fit(); repaint(); }; selection.onClick = [this] { view.fitSelection(data.startSample, data.endSample); repaint(); };
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
    }
    std::function<void()> onBeginEdit;
    std::function<void(int64_t, int64_t)> onTrim;
    void setChopPreview(int slices) { chopSlices = slices; repaint(); }
    void setPad(model::PadData next) {
        if (data.audioData.identity() != next.audioData.identity()) view.setSize(next.audioData.getNumSamples());
        data = std::move(next); repaint();
    }
    void resized() override {
        zoomOut.setBounds(0, 0, 34, 22); zoomIn.setBounds(38, 0, 34, 22); fit.setBounds(80, 0, 54, 22); selection.setBounds(140, 0, 96, 22);
    }
    void paint(juce::Graphics& g) override {
        const auto area = plot(); const int count = data.audioData.getNumSamples();
        g.setColour(juce::Colour(0xff111416)); g.fillRoundedRectangle(area.toFloat(), 4);
        g.setColour(juce::Colour(0xff8d9a9d)); g.setFont(11);
        g.drawText(juce::String(view.zoomFactor(), 1) + "x", juce::Rectangle<int>{getWidth() - 75, 0, 75, 22}, juce::Justification::centredRight);
        if (!data.hasSample()) { g.drawText("DROP WAV / FLAC / AIFF", area, juce::Justification::centred); return; }
        const float startX = xAt(data.startSample), endX = xAt(data.endSample);
        {
            juce::Graphics::ScopedSaveState saved(g); g.reduceClipRegion(area);
            g.setColour(juce::Colour(0xffbcf468));
            for (int x = 0; x < area.getWidth(); ++x) {
                const int first = static_cast<int>(view.sampleAt(static_cast<double>(x) / area.getWidth()));
                const int last = std::min(count, std::max(first + 1, static_cast<int>(view.sampleAt(static_cast<double>(x + 1) / area.getWidth()))));
                const auto peak = data.audioData.range(first, last);
                const float scale = area.getHeight() * 0.38f;
                g.drawVerticalLine(area.getX() + x, area.getCentreY() - std::min(1.0f, peak.high) * scale, area.getCentreY() - std::max(-1.0f, peak.low) * scale);
            }
            g.setColour(juce::Colours::black.withAlpha(0.6f));
            if (startX > area.getX()) g.fillRect(juce::Rectangle<float>(static_cast<float>(area.getX()), static_cast<float>(area.getY()), startX - area.getX(), static_cast<float>(area.getHeight())));
            if (endX < area.getRight()) g.fillRect(juce::Rectangle<float>(endX, static_cast<float>(area.getY()), area.getRight() - endX, static_cast<float>(area.getHeight())));
            g.setColour(juce::Colour(0xffff9260));
            g.drawLine(startX, static_cast<float>(area.getBottom()), xAt(data.startSample + static_cast<int64_t>(data.sampleRate * data.fadeInMs / 1000)), static_cast<float>(area.getY()), 1);
            g.drawLine(xAt(data.endSample - static_cast<int64_t>(data.sampleRate * data.fadeOutMs / 1000)), static_cast<float>(area.getY()), endX, static_cast<float>(area.getBottom()), 1);
            drawMarker(g, startX, "IN", area); drawMarker(g, endX, "OUT", area);
            if (chopSlices > 0 && data.endSample - data.startSample >= chopSlices) {
                g.setColour(juce::Colours::white.withAlpha(0.55f));
                for (int i = 1; i < chopSlices; ++i) {
                    const float x = xAt(data.startSample + (data.endSample - data.startSample) * i / chopSlices);
                    g.drawLine(x, static_cast<float>(area.getY() + 18), x, static_cast<float>(area.getBottom()), 1);
                }
            }
        }
        g.setColour(juce::Colour(0xff8d9a9d)); g.setFont(10);
        g.drawText(juce::String(view.visibleStart() / data.sampleRate, 3) + " s", juce::Rectangle<int>{0, getHeight() - 15, 130, 15}, juce::Justification::centredLeft);
        g.drawText(juce::String((view.visibleStart() + view.visibleLength()) / data.sampleRate, 3) + " s", juce::Rectangle<int>{getWidth() - 130, getHeight() - 15, 130, 15}, juce::Justification::centredRight);
    }
    void mouseDown(const juce::MouseEvent& event) override {
        if (!data.hasSample() || !plot().contains(event.getPosition())) return;
        panning = event.mods.isAltDown() || event.mods.isMiddleButtonDown(); lastX = event.position.x;
        if (panning) return;
        editing = true; startMarker = std::abs(event.position.x - xAt(data.startSample)) <= std::abs(event.position.x - xAt(data.endSample));
        if (onBeginEdit) onBeginEdit(); moveMarker(event.position.x);
    }
    void mouseDrag(const juce::MouseEvent& event) override {
        if (panning) { view.pan((lastX - event.position.x) / plot().getWidth()); lastX = event.position.x; repaint(); }
        else if (editing) moveMarker(event.position.x);
    }
    void mouseUp(const juce::MouseEvent&) override { editing = panning = false; }
    void mouseDoubleClick(const juce::MouseEvent&) override { view.fitSelection(data.startSample, data.endSample); repaint(); }
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override {
        if (event.mods.isShiftDown() || std::abs(wheel.deltaX) > std::abs(wheel.deltaY)) view.pan(-(wheel.deltaX + wheel.deltaY) * 0.5);
        else view.zoom(std::pow(2.0, wheel.deltaY * 3), (event.position.x - plot().getX()) / plot().getWidth());
        repaint();
    }
private:
    model::PadData data;
    WaveformViewport view;
    juce::TextButton zoomIn{"+"}, zoomOut{"-"}, fit{"FIT"}, selection{"SELECTION"};
    bool panning = false, editing = false, startMarker = true;
    float lastX = 0;
    int chopSlices = 0;
    juce::Rectangle<int> plot() const { return {0, 28, getWidth(), std::max(1, getHeight() - 46)}; }
    float xAt(int64_t sample) const { return static_cast<float>(plot().getX() + view.fractionAt(sample) * plot().getWidth()); }
    static void drawMarker(juce::Graphics& g, float x, const juce::String& label, juce::Rectangle<int> area) {
        g.drawLine(x, static_cast<float>(area.getY()), x, static_cast<float>(area.getBottom()), 2);
        g.setFont(10); g.drawText(label, static_cast<int>(x) + (label == "IN" ? 3 : -30), area.getY(), 28, 14, juce::Justification::centred);
    }
    void moveMarker(float x) {
        const auto sample = view.sampleAt((x - plot().getX()) / plot().getWidth());
        WaveformViewport::moveMarker(startMarker, sample, data.audioData.getNumSamples(), data.startSample, data.endSample);
        if (onTrim) onTrim(data.startSample, data.endSample); repaint();
    }
};
}
