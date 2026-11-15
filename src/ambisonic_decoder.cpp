#include "ambisonic_decoder.h"
#include "temporary_file.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <ambi_bin.h>
#include <mysofa.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ambi {

namespace {

std::string_view attribute(const MYSOFA_ATTRIBUTE* entry, std::string_view key)
{
    for (; entry; entry = entry->next) {
        if (entry->name && entry->value && key == entry->name) {
            return entry->value;
        }
    }
    return {};
}

bool finite_samples(const MYSOFA_ARRAY& array, std::size_t count)
{
    return array.values && array.elements == count &&
           std::all_of(array.values, array.values + count, [](float value) { return std::isfinite(value); });
}

void check_sofa(std::span<const unsigned char> data, int order)
{
    int error = 0;
    std::unique_ptr<MYSOFA_HRTF, decltype(&mysofa_free)> hrir(
        mysofa_load_data(reinterpret_cast<const char*>(data.data()), data.size(), &error), mysofa_free);
    if (!hrir || error != MYSOFA_OK) {
        throw std::runtime_error("Could not read the SOFA file (libmysofa error " + std::to_string(error) + ").");
    }
    if (attribute(hrir->attributes, "SOFAConventions") != "SimpleFreeFieldHRIR" ||
        attribute(hrir->attributes, "DataType") != "FIR" || hrir->R != 2 ||
        hrir->E != 1 || hrir->C != 3 || hrir->N == 0) {
        throw std::runtime_error("Expected a two-receiver SimpleFreeFieldHRIR SOFA file.");
    }
    if (hrir->M < channel_count(order)) {
        throw std::runtime_error("The SOFA file has too few measured directions for this decoding order.");
    }
    const auto count = static_cast<std::size_t>(hrir->M);
    auto units = attribute(hrir->SourcePosition.attributes, "Units");
    const bool azimuth_in_degrees = units.starts_with("degree,");
    if (azimuth_in_degrees) {
        units.remove_prefix(7);
        const auto start = units.find_first_not_of(' ');
        units.remove_prefix(start == std::string_view::npos ? units.size() : start);
    }
    const auto max_ir_samples = static_cast<std::size_t>(std::numeric_limits<int>::max());
    if (attribute(hrir->SourcePosition.attributes, "Type") != "spherical" ||
        !azimuth_in_degrees || !units.starts_with("degree,") ||
        !finite_samples(hrir->SourcePosition, count * 3) ||
        count > max_ir_samples / hrir->R / hrir->N ||
        !finite_samples(hrir->DataIR, count * hrir->R * hrir->N) ||
        !finite_samples(hrir->DataSamplingRate, 1) || hrir->DataSamplingRate.values[0] < 1 ||
        static_cast<double>(hrir->DataSamplingRate.values[0]) >= std::numeric_limits<int>::max()) {
        throw std::runtime_error("The SOFA file needs spherical directions in degrees, valid HRIR samples, and one sample rate.");
    }
    const auto& delay = hrir->DataDelay;
    if (delay.elements && (!finite_samples(delay, delay.elements) ||
        !std::all_of(delay.values, delay.values + delay.elements,
                     [&delay](float value) { return value == delay.values[0]; }))) {
        throw std::runtime_error("SAF needs direction- and ear-dependent SOFA delays baked into Data.IR.");
    }
}

std::string saf_path(const std::filesystem::path& path)
{
#ifdef _WIN32
    wchar_t short_name[MAX_PATH]{};
    const auto length = GetShortPathNameW(path.c_str(), short_name, MAX_PATH);
    if (length && length < MAX_PATH) {
        return std::filesystem::path(short_name).string();
    }
#endif
    return path.string();
}

} // namespace

AmbisonicDecoder::AmbisonicDecoder(int order, const DecoderSettings& settings,
                                   std::span<const unsigned char> sofa)
{
    constexpr std::array presets{SH_ORDER_FIRST, SH_ORDER_SECOND, SH_ORDER_THIRD,
                                 SH_ORDER_FOURTH, SH_ORDER_FIFTH};
    if (order < min_order || order > max_order) {
        throw std::invalid_argument("Expected Ambisonic order 1 through 5.");
    }
    void* handle = nullptr;
    ambi_bin_create(&handle);
    handle_.reset(handle);
    if (!handle) {
        throw std::runtime_error("Could not create SAF AmbiBIN decoder.");
    }

    ambi_bin_setInputOrderPreset(handle, presets[order - min_order]);
    ambi_bin_setChOrder(handle, CH_ACN);
    ambi_bin_setNormType(handle, NORM_SN3D);
    constexpr std::array methods{DECODING_METHOD_LS, DECODING_METHOD_LSDIFFEQ,
                                 DECODING_METHOD_SPR, DECODING_METHOD_TA, DECODING_METHOD_MAGLS};
    constexpr std::array preprocessing{HRIR_PREPROC_OFF, HRIR_PREPROC_EQ,
                                       HRIR_PREPROC_PHASE, HRIR_PREPROC_ALL};
    ambi_bin_setDecodingMethod(handle, methods.at(static_cast<std::size_t>(settings.method)));
    ambi_bin_setHRIRsPreProc(handle, preprocessing.at(static_cast<std::size_t>(settings.preprocessing)));
    ambi_bin_setEnableMaxRE(handle, settings.max_re);
    ambi_bin_setEnableDiffuseMatching(handle, settings.diffuse_covariance);
    ambi_bin_setEnableTruncationEQ(handle, settings.truncation_eq);
    ambi_bin_setEnableRotation(handle, 0);
    ambi_bin_init(handle, sample_rate);
    if (sofa.empty()) {
        ambi_bin_initCodec(handle);
    } else {
        check_sofa(sofa, order);
        const TemporaryFile file(std::filesystem::temp_directory_path());
        std::ofstream output(file.path(), std::ios::binary);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output.write(reinterpret_cast<const char*>(sofa.data()), static_cast<std::streamsize>(sofa.size()));
        output.close();
        const auto path = saf_path(file.path());
        ambi_bin_setSofaFilePath(handle, path.c_str());
        ambi_bin_initCodec(handle);
        if (ambi_bin_getUseDefaultHRIRsflag(handle)) {
            throw std::runtime_error("SAF could not initialize the requested SOFA HRIRs.");
        }
    }
    if (ambi_bin_getCodecStatus(handle) != CODEC_STATUS_INITIALISED) {
        throw std::runtime_error("Could not initialize SAF AmbiBIN codec.");
    }
}

void AmbisonicDecoder::Deleter::operator()(void* handle) const noexcept
{
    ambi_bin_destroy(&handle);
}

std::size_t AmbisonicDecoder::block_size() noexcept
{
    return static_cast<std::size_t>(ambi_bin_getFrameSize());
}

std::size_t AmbisonicDecoder::latency() noexcept
{
    return static_cast<std::size_t>(ambi_bin_getProcessingDelay());
}

ListenerRotation listener_rotation(const CameraBasis& basis)
{
    // Reject scale, shear, reflection and non-finite components before updating SAF.
    constexpr double tolerance = 1e-5;
    for (std::size_t i = 0; i < basis.size(); ++i) {
        for (std::size_t j = i; j < basis.size(); ++j) {
            const auto dot = std::inner_product(basis[i].begin(), basis[i].end(), basis[j].begin(), 0.0);
            if (!std::isfinite(dot) || std::abs(dot - (i == j ? 1.0 : 0.0)) > tolerance) {
                throw std::invalid_argument("Listener basis must be an orthonormal rotation.");
            }
        }
    }
    const auto& [x, y, z] = basis;
    const auto determinant = x[0] * (y[1] * z[2] - y[2] * z[1])
                           + x[1] * (y[2] * z[0] - y[0] * z[2])
                           + x[2] * (y[0] * z[1] - y[1] * z[0]);
    if (determinant < 0) {
        throw std::invalid_argument("Listener basis must be right-handed.");
    }

    // R = C * B^T * C^T. C maps Godot XYZ to SAF forward/left/up as (-Z, -X, +Y).
    constexpr std::array axis{2, 0, 1};
    constexpr std::array sign{-1.0, -1.0, 1.0};
    CameraBasis rotation{};
    for (std::size_t row = 0; row < rotation.size(); ++row) {
        for (std::size_t col = 0; col < rotation[row].size(); ++col) {
            rotation[row][col] = sign[row] * sign[col] * basis[axis[row]][axis[col]];
        }
    }

    // SAF's default order gives Rx(-roll) * Ry(-pitch) * Rz(-yaw) in active form.
    const auto horizontal = std::hypot(rotation[0][0], rotation[0][1]);
    const auto pitch = std::atan2(-rotation[0][2], horizontal);
    const bool singular = horizontal < 1e-7;
    const auto yaw = singular ? std::atan2(-rotation[1][0], rotation[1][1])
                              : std::atan2(rotation[0][1], rotation[0][0]);
    const auto roll = singular ? 0.0 : std::atan2(rotation[1][2], rotation[2][2]);
    constexpr double degrees = 180.0 / std::numbers::pi;
    return {static_cast<float>(yaw * degrees), static_cast<float>(pitch * degrees),
            static_cast<float>(roll * degrees)};
}

void AmbisonicDecoder::set_listener_rotation(const ListenerRotation& rotation) noexcept
{
    auto* handle = handle_.get();
    ambi_bin_setRPYflag(handle, 0);
    ambi_bin_setYaw(handle, rotation.yaw);
    ambi_bin_setPitch(handle, rotation.pitch);
    ambi_bin_setRoll(handle, rotation.roll);
    ambi_bin_setEnableRotation(handle, 1);
}

void AmbisonicDecoder::process(std::span<const float* const> input,
                         const std::array<float*, stereo_channels>& output) noexcept
{
    ambi_bin_process(handle_.get(), input.data(), output.data(),
                     static_cast<int>(input.size()), static_cast<int>(output.size()),
                     ambi_bin_getFrameSize());
}

} // namespace ambi
