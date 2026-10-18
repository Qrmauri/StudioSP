#include "PluginProcessor.h"
#include "ui/MainComponent.h"
juce::AudioProcessorEditor* StudioSPProcessor::createEditor() { return new studiosp::ui::MainComponent(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new StudioSPProcessor(); }
