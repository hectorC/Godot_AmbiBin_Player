#include "ambisonic_decoder.h"
#include "wav_io.h"

#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_playback_resampled.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ambi {
namespace {

using namespace godot;

class SofaHrir : public Resource {
    GDCLASS(SofaHrir, Resource)

public:
    void set_data(const PackedByteArray& data) { data_ = data; }
    PackedByteArray get_data() const { return data_; }

protected:
    static void _bind_methods()
    {
        ClassDB::bind_method(D_METHOD("set_data", "data"), &SofaHrir::set_data);
        ClassDB::bind_method(D_METHOD("get_data"), &SofaHrir::get_data);
        ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "data", PROPERTY_HINT_NONE,
                                  "", PROPERTY_USAGE_STORAGE), "set_data", "get_data");
    }

private:
    PackedByteArray data_;
};

struct DecoderConfiguration {
    int order = 0;
    DecoderSettings settings;
    PackedByteArray sofa;

    std::unique_ptr<AmbisonicDecoder> make_decoder(int input_order) const
    {
        return std::make_unique<AmbisonicDecoder>(
            order ? order : input_order, settings,
            std::span{sofa.ptr(), static_cast<std::size_t>(sofa.size())});
    }
};

struct PlaybackControls {
    std::atomic<bool> loop{false};

    // One scene-thread writer, any number of playback readers. A racing reader
    // keeps its last pose for this block instead of retrying on the audio thread.
    std::atomic<uint64_t> revision{0};
    std::atomic<float> yaw{0}, pitch{0}, roll{0};

    void publish(const ListenerRotation& pose) noexcept
    {
        revision.fetch_add(1);
        yaw.store(pose.yaw);
        pitch.store(pose.pitch);
        roll.store(pose.roll);
        revision.fetch_add(1);
    }

    void apply_pose(AmbisonicDecoder& decoder, uint64_t& applied) const noexcept
    {
        const auto before = revision.load();
        if ((before & 1) || before == applied) {
            return;
        }
        const ListenerRotation pose{yaw.load(), pitch.load(), roll.load()};
        if (revision.load() == before) {
            decoder.set_listener_rotation(pose);
            applied = before;
        }
    }
};

static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::size_t>::is_always_lock_free);
static_assert(std::atomic<float>::is_always_lock_free);
static_assert(std::atomic<int32_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

class AmbisonicPlayback : public AudioStreamPlaybackResampled {
    GDCLASS(AmbisonicPlayback, AudioStreamPlaybackResampled)

public:
    void prepare(std::shared_ptr<const AmbisonicAudio> audio, std::shared_ptr<const PlaybackControls> controls,
                 const DecoderConfiguration& configuration, std::unique_ptr<AmbisonicDecoder> decoder)
    {
        audio_ = std::move(audio);
        controls_ = std::move(controls);
        block_size_ = AmbisonicDecoder::block_size();
        input_.resize(audio_->channels.size());
        input_block_.resize(input_.size());
        for (std::size_t channel = 0; channel < input_.size(); ++channel) {
            input_block_[channel].resize(block_size_);
            input_[channel] = input_block_[channel].data();
        }
        for (std::size_t ear = 0; ear < output_.size(); ++ear) {
            output_block_[ear].resize(block_size_);
            output_[ear] = output_block_[ear].data();
        }
        configuration_ = configuration;
        decoder_ = std::move(decoder);
    }

    void _start(double from_pos) override
    {
        if (!decoder_ || !std::isfinite(from_pos)) {
            return;
        }
        // AudioStreamPlayer instantiates a new playback per Play call.
        // Direct reuse through AudioStreamPlayback.start() gets fresh filter state.
        if (started_) {
            playing_.store(false);
            try {
                decoder_ = configuration_.make_decoder(audio_->order);
            } catch (const std::exception& error) {
                UtilityFunctions::push_error(error.what());
                return;
            }
        }
        started_ = true;
        applied_pose_ = 0;
        const auto frame_count = audio_->channels.front().size();
        cursor_ = static_cast<std::size_t>(std::clamp(from_pos * sample_rate, 0.0,
                                                      static_cast<double>(frame_count)));
        tail_left_ = AmbisonicDecoder::latency();
        block_read_ = block_valid_ = 0;
        position_.store(cursor_);
        loop_count_.store(0);
        playing_.store(true);
        begin_resample();
    }

    void _stop() override { playing_.store(false); }
    bool _is_playing() const override { return playing_.load(); }
    float _get_stream_sampling_rate() const override { return sample_rate; }
    int32_t _get_loop_count() const override { return loop_count_.load(); }

    double _get_playback_position() const override
    {
        return static_cast<double>(position_.load()) / sample_rate;
    }

    int32_t _mix_resampled(AudioFrame* buffer, int32_t frames) override
    {
        int32_t written = 0;
        if (playing_.load()) {
            while (written < frames) {
                if (block_read_ == block_valid_ && !render_block()) {
                    break;
                }
                const auto count = std::min(block_valid_ - block_read_,
                                            static_cast<std::size_t>(frames - written));
                for (std::size_t frame = 0; frame < count; ++frame) {
                    buffer[written++] = {output_[0][block_read_], output_[1][block_read_]};
                    ++block_read_;
                }
            }
            position_.store(cursor_);
        }
        if (written < frames) {
            std::fill_n(buffer + written, frames - written, AudioFrame{0, 0});
            playing_.store(false);
        }
        return written;
    }

protected:
    static void _bind_methods() {}

private:
    bool render_block() noexcept
    {
        const auto& channels = audio_->channels;
        const auto frame_count = channels.front().size();
        const bool loop = controls_->loop.load();
        block_read_ = block_valid_ = 0;
        while (block_valid_ < block_size_) {
            if (cursor_ == frame_count && loop) {
                cursor_ = 0;
                if (loop_count_.load() < std::numeric_limits<int32_t>::max()) {
                    loop_count_.fetch_add(1);
                }
                tail_left_ = AmbisonicDecoder::latency();
            }
            if (cursor_ < frame_count) {
                for (std::size_t channel = 0; channel < input_.size(); ++channel) {
                    input_block_[channel][block_valid_] = channels[channel][cursor_];
                }
                ++cursor_;
            } else if (tail_left_) {
                for (auto& channel : input_block_) {
                    channel[block_valid_] = 0;
                }
                --tail_left_;
            } else {
                break;
            }
            ++block_valid_;
        }
        if (!block_valid_) {
            return false;
        }
        for (auto& channel : input_block_) {
            std::fill(channel.begin() + block_valid_, channel.end(), 0.0f);
        }
        controls_->apply_pose(*decoder_, applied_pose_);
        decoder_->process(input_, output_);
        return true;
    }

    std::shared_ptr<const AmbisonicAudio> audio_;
    std::shared_ptr<const PlaybackControls> controls_;
    DecoderConfiguration configuration_;
    std::unique_ptr<AmbisonicDecoder> decoder_;
    std::vector<std::vector<float>> input_block_;
    std::array<std::vector<float>, stereo_channels> output_block_;
    std::vector<const float*> input_;
    std::array<float*, stereo_channels> output_{};
    std::size_t block_size_ = 0, block_read_ = 0, block_valid_ = 0;
    std::size_t cursor_ = 0, tail_left_ = 0;
    uint64_t applied_pose_ = 0;
    bool started_ = false;
    std::atomic<int32_t> loop_count_{0};
    std::atomic<std::size_t> position_{0};
    std::atomic<bool> playing_{false};
};

class AmbisonicStream : public AudioStream {
    GDCLASS(AmbisonicStream, AudioStream)

public:
    String load_file(const String& path)
    {
        try {
            const auto file = FileAccess::open(path, FileAccess::READ);
            if (file.is_null()) {
                return "Could not open " + path;
            }
            const auto length = file->get_length();
            if (length > std::numeric_limits<uint32_t>::max()) {
                return "Input exceeds the RIFF WAV size limit.";
            }
            const auto bytes = file->get_buffer(static_cast<int64_t>(length));
            if (static_cast<uint64_t>(bytes.size()) != length) {
                return "Could not read the entire WAV.";
            }
            audio_ = std::make_shared<const AmbisonicAudio>(
                read_ambisonic_wav({bytes.ptr(), static_cast<std::size_t>(bytes.size())}));
            prepared_decoder_.reset();
            return {};
        } catch (const std::exception& error) {
            return String::utf8(error.what());
        }
    }

    void set_loop(bool loop) { controls_->loop.store(loop); }
    int get_ambisonic_order() const { return audio_ ? audio_->order : 0; }

    String configure_decoder(int order, int method, bool max_re, bool diffuse_covariance,
                             bool truncation_eq, int preprocessing, const PackedByteArray& sofa)
    {
        if (!audio_) {
            return "Load an Ambisonic WAV before configuring its decoder.";
        }
        if (order < 0 || order > max_order || method < 0 ||
            method > static_cast<int>(DecoderSettings::Method::magls) || preprocessing < 0 ||
            preprocessing > static_cast<int>(DecoderSettings::Preprocessing::both)) {
            return "Invalid AmbiBIN decoder setting.";
        }
        DecoderConfiguration candidate{
            order, {static_cast<DecoderSettings::Method>(method),
                    static_cast<DecoderSettings::Preprocessing>(preprocessing),
                    max_re, diffuse_covariance, truncation_eq}, sofa
        };
        try {
            auto decoder = candidate.make_decoder(audio_->order);
            configuration_ = std::move(candidate);
            prepared_decoder_ = std::move(decoder);
            return {};
        } catch (const std::exception& error) {
            return String::utf8(error.what());
        }
    }

    bool set_listener_basis(const Basis& basis)
    {
        CameraBasis axes{};
        for (int axis = 0; axis < 3; ++axis) {
            const auto column = basis.get_column(axis);
            axes[axis] = {column.x, column.y, column.z};
        }
        try {
            controls_->publish(listener_rotation(axes));
            return true;
        } catch (const std::invalid_argument&) {
            return false;
        }
    }

    double _get_length() const override
    {
        return audio_ ? static_cast<double>(audio_->channels.front().size()) / sample_rate : 0.0;
    }

    Ref<AudioStreamPlayback> _instantiate_playback() const override
    {
        if (!audio_) {
            return {};
        }
        try {
            Ref<AmbisonicPlayback> playback;
            playback.instantiate();
            auto decoder = prepared_decoder_ ? std::move(prepared_decoder_) :
                                               configuration_.make_decoder(audio_->order);
            playback->prepare(audio_, controls_, configuration_, std::move(decoder));
            return playback;
        } catch (const std::exception& error) {
            UtilityFunctions::push_error(error.what());
            return {};
        }
    }

protected:
    static void _bind_methods()
    {
        ClassDB::bind_method(D_METHOD("load_file", "path"), &AmbisonicStream::load_file);
        ClassDB::bind_method(D_METHOD("set_loop", "enabled"), &AmbisonicStream::set_loop);
        ClassDB::bind_method(D_METHOD("get_ambisonic_order"), &AmbisonicStream::get_ambisonic_order);
        ClassDB::bind_method(D_METHOD("configure_decoder", "order", "method", "max_re", "diffuse_covariance",
                                      "truncation_eq", "preprocessing", "sofa"), &AmbisonicStream::configure_decoder);
        ClassDB::bind_method(D_METHOD("set_listener_basis", "basis"), &AmbisonicStream::set_listener_basis);
    }

private:
    std::shared_ptr<const AmbisonicAudio> audio_;
    std::shared_ptr<PlaybackControls> controls_ = std::make_shared<PlaybackControls>();
    DecoderConfiguration configuration_;
    mutable std::unique_ptr<AmbisonicDecoder> prepared_decoder_;
};

void initialize(ModuleInitializationLevel level)
{
    if (level == MODULE_INITIALIZATION_LEVEL_SCENE) {
        GDREGISTER_CLASS(SofaHrir);
        GDREGISTER_INTERNAL_CLASS(AmbisonicPlayback);
        GDREGISTER_CLASS(AmbisonicStream);
    }
}

} // namespace
} // namespace ambi

extern "C" GDExtensionBool GDE_EXPORT ambisonic_player_init(
    GDExtensionInterfaceGetProcAddress get_proc_address,
    GDExtensionClassLibraryPtr library,
    GDExtensionInitialization* initialization)
{
    godot::GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
    init.register_initializer(ambi::initialize);
    init.set_minimum_library_initialization_level(godot::MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}
