#pragma once
#include <iostream>
#include <cstdlib>
extern thread_local bool auditingAudio;
extern thread_local int audioAllocations, audioDeletes;
inline void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
