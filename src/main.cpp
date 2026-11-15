#include "ambisonic_decoder.h"
#include "wav_io.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
    constexpr int basis_argc = 13;
#ifdef _WIN32
    const bool has_basis = argc == basis_argc && std::wstring_view(argv[3]) == L"--listener-basis";
#else
    const bool has_basis = argc == basis_argc && std::string_view(argv[3]) == "--listener-basis";
#endif
    if (argc != 3 && !has_basis) {
        std::cerr << "Usage: ambi_render <48 kHz ambiX.wav, order 1-5> <stereo.wav>\n"
                  << "       Input encoding: 16-bit PCM, 24-bit PCM, or 32-bit IEEE float.\n"
                  << "       [--listener-basis x.x x.y x.z y.x y.y y.z z.x z.y z.z]\n";
        return 2;
    }

    try {
        ambi::CameraBasis basis{};
        if (has_basis) {
            int component = 4;
            for (auto& column : basis) {
                for (auto& value : column) {
                    const std::basic_string token(argv[component++]);
                    std::size_t parsed = 0;
                    value = std::stod(token, &parsed);
                    if (parsed != token.size()) {
                        throw std::invalid_argument("Expected numeric listener basis components.");
                    }
                }
            }
        }
        const std::filesystem::path input_path(argv[1]);
        const std::filesystem::path output_path(argv[2]);
        const auto audio = ambi::read_ambisonic_wav(input_path);
        const auto& channels = audio.channels;
        if (std::filesystem::exists(output_path) &&
            std::filesystem::equivalent(input_path, output_path)) {
            throw std::runtime_error("Input and output must be different files.");
        }

        ambi::AmbisonicDecoder decoder(audio.order);
        if (has_basis) {
            decoder.set_listener_rotation(ambi::listener_rotation(basis));
        }
        const auto block_size = decoder.block_size();
        const auto input_frames = channels.front().size();
        const auto output_frames = input_frames + decoder.latency();
        std::vector<float> render(output_frames * ambi::stereo_channels);
        std::vector<std::vector<float>> input_block(channels.size());
        std::array<std::vector<float>, ambi::stereo_channels> output_block;
        std::vector<const float*> input(channels.size());
        std::array<float*, ambi::stereo_channels> output{};
        for (std::size_t ch = 0; ch < input.size(); ++ch) {
            input_block[ch].resize(block_size);
            input[ch] = input_block[ch].data();
        }
        for (std::size_t ear = 0; ear < output.size(); ++ear) {
            output_block[ear].resize(block_size);
            output[ear] = output_block[ear].data();
        }

        float peak = 0;
        for (std::size_t offset = 0; offset < output_frames; offset += block_size) {
            const auto count = offset < input_frames ? std::min(block_size, input_frames - offset) : 0;
            for (std::size_t ch = 0; ch < input.size(); ++ch) {
                if (count) {
                    std::copy_n(channels[ch].data() + offset, count, input_block[ch].data());
                }
                std::fill(input_block[ch].begin() + count, input_block[ch].end(), 0.0f);
            }
            decoder.process(input, output);
            const auto available = std::min(block_size, output_frames - offset);
            for (std::size_t frame = 0; frame < available; ++frame) {
                for (std::size_t ear = 0; ear < output.size(); ++ear) {
                    const auto sample = output[ear][frame];
                    if (!std::isfinite(sample)) {
                        throw std::runtime_error("SAF produced a non-finite output sample.");
                    }
                    render[(offset + frame) * output.size() + ear] = sample;
                    peak = std::max(peak, std::abs(sample));
                }
            }
        }
        ambi::write_stereo_wav(output_path, render);
        std::cout << "Rendered " << input_frames << " order " << audio.order << " ambiX frames to " << output_frames
                  << " stereo frames at " << ambi::sample_rate << " Hz. SAF block: " << block_size
                  << ", reported latency: " << decoder.latency() << " frames, peak: " << peak << ".\n";
        if (peak > 1.0f) {
            std::cerr << "Peak exceeds digital full scale. Float output retains the original levels.\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ambi_render: " << error.what() << '\n';
        return 1;
    }
}
