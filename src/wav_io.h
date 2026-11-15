#pragma once

#include "ambisonic_decoder.h"

#include <filesystem>
#include <span>
#include <vector>

namespace ambi {

struct AmbisonicAudio {
    int order;
    std::vector<std::vector<float>> channels;
};

AmbisonicAudio read_ambisonic_wav(const std::filesystem::path& path);
AmbisonicAudio read_ambisonic_wav(std::span<const unsigned char> bytes);
void write_stereo_wav(const std::filesystem::path& path, const std::vector<float>& samples);

} // namespace ambi
