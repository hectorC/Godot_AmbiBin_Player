#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <span>

namespace ambi {

inline constexpr int sample_rate = 48000;
inline constexpr int min_order = 1;
inline constexpr int max_order = 5;
inline constexpr std::size_t stereo_channels = 2;

constexpr std::size_t channel_count(int order)
{
    const auto bands = static_cast<std::size_t>(order + 1);
    return bands * bands;
}

// Columns of the camera's local-to-world rotation, in Godot X, Y, Z coordinates.
using CameraBasis = std::array<std::array<double, 3>, 3>;

struct ListenerRotation {
    float yaw = 0;
    float pitch = 0;
    float roll = 0;
};

struct DecoderSettings {
    enum class Method { ls, ls_diffuse_eq, spr, ta, magls };
    enum class Preprocessing { off, diffuse_eq, phase, both };

    Method method = Method::magls;
    Preprocessing preprocessing = Preprocessing::diffuse_eq;
    bool max_re = true;
    bool diffuse_covariance = false;
    bool truncation_eq = true;
};

// Validate the camera basis and convert it to SAF's angle convention in degrees.
[[nodiscard]] ListenerRotation listener_rotation(const CameraBasis& basis);

class AmbisonicDecoder {
public:
    explicit AmbisonicDecoder(int order, const DecoderSettings& settings = {},
                              std::span<const unsigned char> sofa = {});

    [[nodiscard]] static std::size_t block_size() noexcept;
    [[nodiscard]] static std::size_t latency() noexcept;

    // Apply the converted pose on the processing thread, between blocks.
    void set_listener_rotation(const ListenerRotation& rotation) noexcept;

    // Input contains (order + 1)^2 ACN/SN3D channels of block_size() samples each.
    void process(std::span<const float* const> input,
                 const std::array<float*, stereo_channels>& output) noexcept;

private:
    struct Deleter {
        void operator()(void* handle) const noexcept;
    };

    std::unique_ptr<void, Deleter> handle_;
};

} // namespace ambi
