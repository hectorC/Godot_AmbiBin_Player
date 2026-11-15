#include "wav_io.h"

#include "temporary_file.h"

#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>

namespace ambi {
namespace {

struct WavReader {
    explicit WavReader(const std::filesystem::path& path)
    {
#ifdef _WIN32
        const auto opened = drwav_init_file_ex_w(&wav, path.c_str(), inspect_chunk, this, 0, nullptr);
#else
        const auto opened = drwav_init_file_ex(&wav, path.c_str(), inspect_chunk, this, 0, nullptr);
#endif
        if (!opened) {
            throw std::runtime_error("Could not open input as a WAV file.");
        }
    }

    explicit WavReader(std::span<const unsigned char> bytes)
    {
        if (!drwav_init_memory_ex(&wav, bytes.data(), bytes.size(), inspect_chunk, this, 0, nullptr)) {
            throw std::runtime_error("Could not read input as a WAV file.");
        }
    }

    ~WavReader() { drwav_uninit(&wav); }
    WavReader(const WavReader&) = delete;
    WavReader& operator=(const WavReader&) = delete;

    drwav wav{};
    std::optional<drwav_uint64> declared_data_size;

private:
    static drwav_uint64 inspect_chunk(void* context, drwav_read_proc, drwav_seek_proc, void*,
                                     const drwav_chunk_header* chunk, drwav_container container,
                                     const drwav_fmt*)
    {
        auto& reader = *static_cast<WavReader*>(context);
        // dr_wav clamps a truncated chunk to the physical file size after this callback.
        if (container == drwav_container_riff && !reader.declared_data_size &&
            drwav_fourcc_equal(chunk->id.fourcc, "data")) {
            reader.declared_data_size = chunk->sizeInBytes;
        }
        return 0;
    }
};

bool is_supported_encoding(const drwav& wav)
{
    constexpr std::array<drwav_uint8, 14> wave_subformat_suffix{
        0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71
    };
    const auto container_bits = wav.bitsPerSample;
    auto valid_bits = container_bits;
    auto format = wav.fmt.formatTag;
    if (format == DR_WAVE_FORMAT_EXTENSIBLE) {
        const auto& guid = wav.fmt.subFormat;
        if (!std::equal(wave_subformat_suffix.begin(), wave_subformat_suffix.end(), guid + 2)) {
            return false;
        }
        format = wav.translatedFormatTag;
        valid_bits = wav.fmt.validBitsPerSample;
    }
    if (format == DR_WAVE_FORMAT_IEEE_FLOAT) {
        return container_bits == 32 && valid_bits == 32;
    }
    return format == DR_WAVE_FORMAT_PCM &&
           (valid_bits == 16 || valid_bits == 24) && valid_bits <= container_bits &&
           (container_bits == 16 || container_bits == 24 || container_bits == 32);
}

AmbisonicAudio decode_ambisonic(WavReader& reader)
{
    auto& wav = reader.wav;
    if (wav.container != drwav_container_riff) {
        throw std::runtime_error("Expected a little-endian RIFF WAV file.");
    }
    int order = min_order;
    while (order <= max_order && channel_count(order) != wav.channels) {
        ++order;
    }
    if (order > max_order) {
        throw std::runtime_error("Expected 4, 9, 16, 25, or 36 ambiX channels (orders 1-5).");
    }
    if (wav.sampleRate != sample_rate) {
        throw std::runtime_error("Expected a 48 kHz input WAV.");
    }
    const auto bytes_per_frame = wav.channels * static_cast<std::size_t>(wav.bitsPerSample / 8);
    if (!is_supported_encoding(wav) || wav.fmt.blockAlign != bytes_per_frame) {
        throw std::runtime_error("Expected 16-bit PCM, 24-bit PCM, or 32-bit IEEE float WAV.");
    }
    if (wav.totalPCMFrameCount == 0 || reader.declared_data_size != wav.dataChunkDataSize ||
        wav.dataChunkDataSize != wav.totalPCMFrameCount * bytes_per_frame) {
        throw std::runtime_error("Input WAV contains no complete audio or has truncated data.");
    }

    std::vector<std::vector<float>> channels(wav.channels);
    const auto total_frames = static_cast<std::size_t>(wav.totalPCMFrameCount);
    for (auto& channel : channels) {
        channel.resize(total_frames);
    }

    constexpr std::size_t buffer_frames = 1024;
    std::array<float, buffer_frames * channel_count(max_order)> interleaved{};
    for (std::size_t offset = 0; offset < total_frames;) {
        const auto count = std::min(buffer_frames, total_frames - offset);
        if (drwav_read_pcm_frames_f32(&wav, count, interleaved.data()) != count) {
            throw std::runtime_error("Input WAV ended before all declared frames could be read.");
        }
        for (std::size_t frame = 0; frame < count; ++frame) {
            for (std::size_t ch = 0; ch < channels.size(); ++ch) {
                const auto sample = interleaved[frame * channels.size() + ch];
                if (!std::isfinite(sample)) {
                    throw std::runtime_error("Input WAV contains a non-finite sample.");
                }
                channels[ch][offset + frame] = sample;
            }
        }
        offset += count;
    }
    return {order, std::move(channels)};
}

} // namespace

AmbisonicAudio read_ambisonic_wav(const std::filesystem::path& path)
{
    WavReader reader(path);
    return decode_ambisonic(reader);
}

AmbisonicAudio read_ambisonic_wav(std::span<const unsigned char> bytes)
{
    WavReader reader(bytes);
    return decode_ambisonic(reader);
}

void write_stereo_wav(const std::filesystem::path& path, const std::vector<float>& samples)
{
    constexpr auto max_data_bytes = std::numeric_limits<drwav_uint32>::max() - 36;
    if (samples.size() > max_data_bytes / sizeof(float)) {
        throw std::runtime_error("Stereo output exceeds the RIFF WAV size limit.");
    }
    constexpr drwav_data_format format{
        drwav_container_riff, DR_WAVE_FORMAT_IEEE_FLOAT, stereo_channels, sample_rate, 32
    };
    const auto directory = path.has_parent_path() ? path.parent_path() : std::filesystem::current_path();
    TemporaryFile temporary(directory);
    drwav wav{};
#ifdef _WIN32
    const auto opened = drwav_init_file_write_w(&wav, temporary.path().c_str(), &format, nullptr);
#else
    const auto opened = drwav_init_file_write(&wav, temporary.path().c_str(), &format, nullptr);
#endif
    if (!opened) {
        throw std::runtime_error("Could not open the temporary WAV for writing.");
    }
    const auto frames = samples.size() / stereo_channels;
    const auto written = drwav_write_pcm_frames(&wav, frames, samples.data());
    const auto result = drwav_uninit(&wav);
    if (written != frames || result != DRWAV_SUCCESS) {
        throw std::runtime_error("Could not write the complete output WAV.");
    }
    temporary.replace(path);
}

} // namespace ambi
