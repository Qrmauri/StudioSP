#pragma once
#include <juce_core/juce_core.h>
#include <vector>
#include <algorithm>

namespace studiosp::model {
inline juce::File sampleFolderSettingsFile() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("StudioSP").getChildFile("sample-folders.json");
}
inline std::vector<juce::File> readSampleFolders(const juce::File& settings) {
    std::vector<juce::File> roots;
    const auto json = juce::JSON::parse(settings);
    if (const auto* paths = json.getProperty("folders", {}).getArray())
        for (const auto& path : *paths) {
            if (!path.isString() || !juce::File::isAbsolutePath(path.toString())) continue;
            const juce::File folder(path.toString());
            if (std::find(roots.begin(), roots.end(), folder) == roots.end()) roots.push_back(folder);
        }
    return roots;
}
inline bool writeSampleFolders(const juce::File& settings, const std::vector<juce::File>& roots) {
    juce::Array<juce::var> paths;
    for (const auto& root : roots) paths.add(root.getFullPathName());
    auto object = std::make_unique<juce::DynamicObject>(); object->setProperty("folders", paths);
    const juce::var json(object.release());
    return settings.getParentDirectory().createDirectory().wasOk()
        && settings.replaceWithText(juce::JSON::toString(json));
}
}
