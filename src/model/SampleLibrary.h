#pragma once
#include <juce_core/juce_core.h>
#include <algorithm>
#include <vector>
#include <functional>

namespace studiosp::model {
inline std::vector<juce::File> scanSampleFolders(const std::vector<juce::File>& folders,
                                               const std::function<bool()>& shouldCancel = {}) {
    std::vector<juce::File> found;
    auto pending = folders;
    const auto cancelled = [&] { return shouldCancel && shouldCancel(); };
    while (!pending.empty()) {
        if (cancelled()) return {};
        const auto root = pending.back(); pending.pop_back();
        // Visit directories explicitly so cancellation also works in trees without audio files.
        for (const auto& entry : juce::RangedDirectoryIterator(root, false, "*", juce::File::findFilesAndDirectories,
                                                             juce::File::FollowSymlinks::no)) {
            if (cancelled()) return {};
            const auto file = entry.getFile();
            if (file.isDirectory()) {
                // Do not follow directory links/junctions into cycles or other libraries.
                if (!file.isSymbolicLink()) pending.push_back(file);
            } else if (file.hasFileExtension("wav;flac;aiff;aif")) found.push_back(file);
        }
    }
    if (cancelled()) return {};
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.getFullPathName().compareIgnoreCase(b.getFullPathName()) < 0; });
    found.erase(std::unique(found.begin(), found.end()), found.end());
    if (cancelled()) return {};
    return found;
}
inline std::vector<juce::File> searchSamples(const std::vector<juce::File>& files, const juce::String& query) {
    std::vector<juce::File> result;
    for (const auto& file : files) if (file.getFullPathName().containsIgnoreCase(query.trim())) result.push_back(file);
    return result;
}
}
