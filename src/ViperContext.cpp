#include "ViperContext.h"
#include "AudioFormat.h"
#include "TelemetryProtocol.h"
#include "log.h"
#include "viper/constants.h"
#include <cerrno>
#include <chrono>
#include <cstring>

#define SET(type, ptr, value) (*(type *) (ptr) = (value))

constexpr int32_t kParamGetEnabled = 1;
constexpr int32_t kParamGetConfigure = 2;
constexpr int32_t kParamGetStreaming = 3;
constexpr int32_t kParamGetSamplingRate = 4;
constexpr int32_t kParamGetConvolutionKernelId = 5;
constexpr int32_t kParamGetDriverVersionCode = 6;
constexpr int32_t kParamGetDriverVersionName = 7;
constexpr int32_t kParamGetArchitecture = 8;
constexpr int32_t kParamGetTelemetry = 9;

ViperContext::ViperContext() :
    config_({}),
    disable_reason_(DisableReason::NONE),
    buffer_(std::vector<float>()),
    dry_buffer_(std::vector<float>()),
    previous_buffer_(std::vector<float>()),
    buffer_frame_count_(0),
    enable_(false),
    has_processed_(false) {
    VIPER_LOGI("ViperContext created");
}

void ViperContext::CopyBufferConfig(buffer_config_t *dest, buffer_config_t *src) {
    if (src->mask & EFFECT_CONFIG_BUFFER) {
        dest->buffer = src->buffer;
    }

    if (src->mask & EFFECT_CONFIG_SMP_RATE) {
        dest->sampling_rate = src->sampling_rate;
    }

    if (src->mask & EFFECT_CONFIG_CHANNELS) {
        dest->channels = src->channels;
    }

    if (src->mask & EFFECT_CONFIG_FORMAT) {
        dest->format = src->format;
    }

    if (src->mask & EFFECT_CONFIG_ACC_MODE) {
        dest->access_mode = src->access_mode;
    }

    if (src->mask & EFFECT_CONFIG_PROVIDER) {
        dest->buffer_provider = src->buffer_provider;
    }

    dest->mask |= src->mask;
}

void ViperContext::HandleSetConfig(effect_config_t *new_config) {
    VIPER_LOGI("Checking input and output configuration ...");

    VIPER_LOGI("Input mask: 0x%04X", new_config->input_cfg.mask);
    VIPER_LOGI("Input buffer frame count: %zu", new_config->input_cfg.buffer.frame_count);
    VIPER_LOGI("Input sampling rate: %d", new_config->input_cfg.sampling_rate);
    VIPER_LOGI("Input channels: %d", new_config->input_cfg.channels);
    VIPER_LOGI("Input format: %d", new_config->input_cfg.format);
    VIPER_LOGI("Input access mode: %d", new_config->input_cfg.access_mode);
    VIPER_LOGI("Output mask: 0x%04X", new_config->output_cfg.mask);
    VIPER_LOGI(
        "Output buffer frame count: %zu", new_config->output_cfg.buffer.frame_count
    );
    VIPER_LOGI("Output sampling rate: %d", new_config->output_cfg.sampling_rate);
    VIPER_LOGI("Output channels: %d", new_config->output_cfg.channels);
    VIPER_LOGI("Output format: %d", new_config->output_cfg.format);
    VIPER_LOGI("Output access mode: %d", new_config->output_cfg.access_mode);

    SetDisableReason(DisableReason::UNKNOWN);

    CopyBufferConfig(&config_.input_cfg, &new_config->input_cfg);
    CopyBufferConfig(&config_.output_cfg, &new_config->output_cfg);

    if (config_.input_cfg.buffer.frame_count != config_.output_cfg.buffer.frame_count) {
        VIPER_LOGE(
            "ViPER4Android disabled, reason [in.FC = %zu, out.FC = %zu]",
            config_.input_cfg.buffer.frame_count,
            config_.output_cfg.buffer.frame_count
        );
        SetDisableReason(
            DisableReason::INVALID_FRAME_COUNT, "Input and output frame count mismatch"
        );
        return;
    }

    if (config_.input_cfg.sampling_rate != config_.output_cfg.sampling_rate) {
        VIPER_LOGE(
            "ViPER4Android disabled, reason [in.SR = %d, out.SR = %d]",
            config_.input_cfg.sampling_rate,
            config_.output_cfg.sampling_rate
        );
        SetDisableReason(
            DisableReason::INVALID_SAMPLING_RATE,
            "Input and output sampling rate mismatch"
        );
        return;
    }

    if (!viper::audio::IsSupportedSampleRate(config_.input_cfg.sampling_rate)) {
        VIPER_LOGE(
            "ViPER4Android disabled, unsupported sampling rate [%d]",
            config_.input_cfg.sampling_rate
        );
        SetDisableReason(
            DisableReason::INVALID_SAMPLING_RATE,
            "Unsupported sampling rate: "
                + std::to_string(config_.input_cfg.sampling_rate)
        );
        return;
    }

    if (config_.input_cfg.channels != config_.output_cfg.channels) {
        VIPER_LOGE(
            "ViPER4Android disabled, reason [in.CH = %d, out.CH = %d]",
            config_.input_cfg.channels,
            config_.output_cfg.channels
        );
        SetDisableReason(
            DisableReason::INVALID_CHANNEL_COUNT,
            "Input and output channel count mismatch"
        );
        return;
    }

    if (config_.input_cfg.channels != AUDIO_CHANNEL_OUT_STEREO) {
        VIPER_LOGE("ViPER4Android disabled, reason [CH != 2]");
        SetDisableReason(
            DisableReason::INVALID_CHANNEL_COUNT,
            "Invalid channel count: " + std::to_string(config_.input_cfg.channels)
        );
        return;
    }

    if (!viper::audio::IsSupportedPcmFormat(
            static_cast<audio_format_t>(config_.input_cfg.format)
        )) {
        VIPER_LOGE(
            "ViPER4Android disabled, reason [in.FMT = %d]", config_.input_cfg.format
        );
        SetDisableReason(
            DisableReason::INVALID_FORMAT,
            "Invalid input format: " + std::to_string(config_.input_cfg.format)
        );
        return;
    }

    if (!viper::audio::IsSupportedPcmFormat(
            static_cast<audio_format_t>(config_.output_cfg.format)
        )) {
        VIPER_LOGE(
            "ViPER4Android disabled, reason [out.FMT = %d]", config_.output_cfg.format
        );
        SetDisableReason(
            DisableReason::INVALID_FORMAT,
            "Invalid output format: " + std::to_string(config_.output_cfg.format)
        );
        return;
    }

    VIPER_LOGI("Input and output configuration checked.");
    SetDisableReason(DisableReason::NONE);

    // Processing buffer
    buffer_.assign(
        viper::audio::kMaxBlockFrames * viper::audio::kChannelCount,
        0.0F
    );
    dry_buffer_.assign(
        viper::audio::kMaxBlockFrames * viper::audio::kChannelCount,
        0.0F
    );
    previous_buffer_.assign(
        viper::audio::kMaxBlockFrames * viper::audio::kChannelCount,
        0.0F
    );
    buffer_frame_count_ = viper::audio::kMaxBlockFrames;

    // ViPER
    const viper::audio::DspGraphConfig graph_config{
        config_.input_cfg.sampling_rate,
        viper::audio::kMaxBlockFrames,
        graph_generation_ + 1,
    };
    const bool prepared = graph_slots_.Active() == nullptr
        ? graph_slots_.PrepareInitial(graph_config, parameter_snapshot_, resources_)
        : graph_slots_.PreparePending(graph_config, parameter_snapshot_, resources_);
    if (!prepared) {
        SetDisableReason(DisableReason::UNKNOWN, "Failed to prepare DSP graph");
        return;
    }
    graph_generation_ = graph_config.generation;
    if (graph_slots_.Pending() == nullptr) {
        graph_slots_.Active()->Transition().StartDryToWet();
    }
    if (!analyzer_.Configure(config_.input_cfg.sampling_rate, 2)) {
        VIPER_LOGE("Failed to configure audio analyzer");
    }
}

void ViperContext::DispatchRawParam(
    int param,
    int val1,
    int val2,
    int val3,
    uint32_t arr_size,
    signed char *arr
) {
    const viper::RawParamUpdate parameter_result = viper::UpdateParameterSnapshot(
        parameter_snapshot_, param, val1, val2, val3, arr_size, arr
    );
    resources_.CaptureRaw(param, val1, val2, val3, arr_size, arr);
    if (parameter_result == viper::RawParamUpdate::UPDATED) {
        parameter_mailbox_.Publish(parameter_snapshot_);
        return;
    }

    std::array<viper::audio::DspGraph *, 3> graphs{
        graph_slots_.Active(),
        graph_slots_.Pending(),
        graph_slots_.Previous(),
    };
    for (size_t i = 0; i < graphs.size(); ++i) {
        if (graphs[i] == nullptr) continue;
        bool duplicate = false;
        for (size_t previous = 0; previous < i; ++previous) {
            if (graphs[previous] == graphs[i]) duplicate = true;
        }
        if (!duplicate) {
            graphs[i]->Engine().DispatchRawParam(
                param, val1, val2, val3, arr_size, arr
            );
        }
    }
}

void ViperContext::ResetGraphs() noexcept {
    std::array<viper::audio::DspGraph *, 3> graphs{
        graph_slots_.Active(),
        graph_slots_.Pending(),
        graph_slots_.Previous(),
    };
    for (size_t i = 0; i < graphs.size(); ++i) {
        if (graphs[i] == nullptr) continue;
        bool duplicate = false;
        for (size_t previous = 0; previous < i; ++previous) {
            if (graphs[previous] == graphs[i]) duplicate = true;
        }
        if (!duplicate) {
            graphs[i]->Reset();
            graphs[i]->Transition().Reset();
        }
    }
    graph_slots_.ReleasePrevious();
}

int32_t ViperContext::HandleSetParam(effect_param_t *cmd_param, void *reply_data) {
    // The value offset of an effect parameter is computed by rounding up
    // the parameter size to the next 32 bit alignment.
    const uint32_t offset =
        ((cmd_param->psize + sizeof(int32_t) - 1) / sizeof(int32_t)) * sizeof(int32_t);

    *static_cast<int *>(reply_data) = 0;

    const int param = *reinterpret_cast<int *>(cmd_param->data);
    const int *int_values = reinterpret_cast<int *>(cmd_param->data + offset);
    switch (cmd_param->vsize) {
        case sizeof(int): {
            DispatchRawParam(param, int_values[0], 0, 0, 0, nullptr);
            return 0;
        }
        case sizeof(int) * 2: {
            DispatchRawParam(param, int_values[0], int_values[1], 0, 0, nullptr);
            return 0;
        }
        case sizeof(int) * 3: {
            DispatchRawParam(
                param, int_values[0], int_values[1], int_values[2], 0, nullptr
            );
            return 0;
        }
        case 256:
        case 1024: {
            const uint32_t arr_size =
                *reinterpret_cast<uint32_t *>(cmd_param->data + offset);
            const auto arr = reinterpret_cast<signed char *>(
                cmd_param->data + offset + sizeof(uint32_t)
            );
            DispatchRawParam(param, 0, 0, 0, arr_size, arr);
            return 0;
        }
        case 8192: {
            const int value1 = *reinterpret_cast<int *>(cmd_param->data + offset);
            const uint32_t arr_size =
                *reinterpret_cast<uint32_t *>(cmd_param->data + offset + sizeof(int));
            const auto arr = reinterpret_cast<signed char *>(
                cmd_param->data + offset + sizeof(int) + sizeof(uint32_t)
            );
            DispatchRawParam(param, value1, 0, 0, arr_size, arr);
            return 0;
        }
        default: {
            return -EINVAL;
        }
    }
}

int32_t ViperContext::HandleGetParam(
    effect_param_t *cmd_param, effect_param_t *reply_param, uint32_t *reply_size
) {
    // The value offset of an effect parameter is computed by rounding up
    // the parameter size to the next 32 bit alignment.
    const uint32_t offset =
        ((cmd_param->psize + sizeof(int32_t) - 1) / sizeof(int32_t)) * sizeof(int32_t);

    memcpy(reply_param, cmd_param, sizeof(effect_param_t) + cmd_param->psize);

    switch (*reinterpret_cast<uint32_t *>(cmd_param->data)) {
        case kParamGetEnabled: {
            reply_param->status = 0;
            reply_param->vsize = sizeof(int32_t);
            *reinterpret_cast<int32_t *>(reply_param->data + offset) = enable_;
            *reply_size =
                sizeof(effect_param_t) + reply_param->psize + offset + reply_param->vsize;
            return 0;
        }
        case kParamGetConfigure: {
            reply_param->status = 0;
            reply_param->vsize = sizeof(int32_t);
            *reinterpret_cast<int32_t *>(reply_param->data + offset) =
                disable_reason_ == DisableReason::NONE;
            *reply_size =
                sizeof(effect_param_t) + reply_param->psize + offset + reply_param->vsize;
            return 0;
        }
        case kParamGetStreaming: {
            const auto *active = graph_slots_.Active();
            const uint64_t frames = active != nullptr
                ? active->Engine().GetProcessedFrames()
                : 0;
            const int32_t is_processing =
                (frames != last_streaming_frames_ && frames > 0) ? 1 : 0;
            last_streaming_frames_ = frames;

            reply_param->status = 0;
            reply_param->vsize = sizeof(int32_t);
            *reinterpret_cast<int32_t *>(reply_param->data + offset) = is_processing;
            *reply_size =
                sizeof(effect_param_t) + reply_param->psize + offset + reply_param->vsize;
            return 0;
        }
        case kParamGetSamplingRate: {
            reply_param->status = 0;
            reply_param->vsize = sizeof(uint32_t);
            *reinterpret_cast<uint32_t *>(reply_param->data + offset) =
                graph_slots_.Active() != nullptr
                    ? graph_slots_.Active()->Engine().GetSamplingRate()
                    : 0;
            *reply_size =
                sizeof(effect_param_t) + reply_param->psize + offset + reply_param->vsize;
            return 0;
        }
        case kParamGetConvolutionKernelId: {
            reply_param->status = 0;
            reply_param->vsize = sizeof(uint32_t);
            *reinterpret_cast<uint32_t *>(reply_param->data + offset) =
                graph_slots_.Active() != nullptr
                    ? graph_slots_.Active()->Engine().GetConvolverKernelID()
                    : 0;
            *reply_size =
                sizeof(effect_param_t) + reply_param->psize + offset + reply_param->vsize;
            return 0;
        }
        case kParamGetDriverVersionCode: {
            reply_param->status = 0;
            reply_param->vsize = sizeof(uint32_t);
            *reinterpret_cast<int32_t *>(reply_param->data + offset) = VERSION_CODE;
            *reply_size =
                sizeof(effect_param_t) + reply_param->psize + offset + reply_param->vsize;
            return 0;
        }
        case kParamGetDriverVersionName: {
            reply_param->status = 0;
            reply_param->vsize = strlen(VERSION_NAME);
            memcpy(reply_param->data + offset, VERSION_NAME, reply_param->vsize);
            *reply_size =
                sizeof(effect_param_t) + reply_param->psize + offset + reply_param->vsize;
            return 0;
        }
        case kParamGetArchitecture: {
            reply_param->status = 0;
            reply_param->vsize = sizeof(VIPER_ARCHITECTURE) - 1;
            memcpy(reply_param->data + offset, VIPER_ARCHITECTURE, reply_param->vsize);
            *reply_size =
                sizeof(effect_param_t) + reply_param->psize + offset + reply_param->vsize;
            return 0;
        }
        case kParamGetTelemetry: {
            const uint32_t required_size =
                sizeof(effect_param_t) + offset + sizeof(viper::TelemetryWire);
            if (*reply_size < required_size) {
                *reply_size = required_size;
                return -ENOSPC;
            }

            viper::AnalyzerSnapshot snapshot{};
            if (!analyzer_.ReadTelemetry(&snapshot)) return -ENODATA;
            const viper::TelemetryWire wire = viper::MakeTelemetryWire(snapshot);
            reply_param->status = 0;
            reply_param->vsize = sizeof(wire);
            memcpy(reply_param->data + offset, &wire, sizeof(wire));
            *reply_size = required_size;
            return 0;
        }
        default: {
            return -EINVAL;
        }
    }
}

int32_t ViperContext::HandleCommand(
    uint32_t cmd_code,
    uint32_t cmd_size,
    void *cmd_data,
    uint32_t *reply_size,
    void *reply_data
) {
    const uint32_t rs = reply_size == nullptr ? 0 : *reply_size;
    switch (cmd_code) {
        case EFFECT_CMD_INIT: {
            if (rs != sizeof(int32_t) || reply_data == nullptr) {
                VIPER_LOGE(
                    "EFFECT_CMD_INIT called with invalid reply_size = %d, reply_data = "
                    "%p, expected reply_size = %zu",
                    rs,
                    reply_data,
                    sizeof(int32_t)
                );
                return -EINVAL;
            }
            SET(int32_t, reply_data, 0);
            return 0;
        }
        case EFFECT_CMD_SET_CONFIG: {
            if (cmd_size < sizeof(effect_config_t) || cmd_data == nullptr
                || rs != sizeof(int32_t) || reply_data == nullptr) {
                VIPER_LOGE(
                    "EFFECT_CMD_SET_CONFIG called with invalid cmd_size = %d, cmd_data = "
                    "%p, reply_size = %d, reply_data = %p, expected cmd_size = %zu, "
                    "reply_size = %zu",
                    cmd_size,
                    cmd_data,
                    rs,
                    reply_data,
                    sizeof(effect_config_t),
                    sizeof(int32_t)
                );
                return -EINVAL;
            }
            HandleSetConfig(static_cast<effect_config_t *>(cmd_data));
            SET(int32_t, reply_data, 0);
            return 0;
        }
        case EFFECT_CMD_RESET: {
            if (rs != sizeof(int32_t) || reply_data == nullptr) {
                VIPER_LOGE(
                    "EFFECT_CMD_RESET called with invalid reply_size = %d, reply_data = "
                    "%p, expected reply_size = %zu",
                    rs,
                    reply_data,
                    sizeof(int32_t)
                );
                return -EINVAL;
            }
            ResetGraphs();
            analyzer_.Reset();
            SET(int32_t, reply_data, 0);
            return 0;
        }
        case EFFECT_CMD_ENABLE: {
            if (rs != sizeof(int32_t) || reply_data == nullptr) {
                VIPER_LOGE(
                    "EFFECT_CMD_ENABLE called with invalid reply_size = %d, reply_data = "
                    "%p, expected reply_size = %zu",
                    rs,
                    reply_data,
                    sizeof(int32_t)
                );
                return -EINVAL;
            }
            ResetGraphs();
            analyzer_.Reset();
            if (!buffer_.empty()) {
                memset(buffer_.data(), 0, buffer_.size() * sizeof(float));
            }
            if (!dry_buffer_.empty()) {
                memset(dry_buffer_.data(), 0, dry_buffer_.size() * sizeof(float));
            }
            if (!previous_buffer_.empty()) {
                memset(previous_buffer_.data(), 0, previous_buffer_.size() * sizeof(float));
            }
            has_processed_ = false;
            enable_ = true;
            SET(int32_t, reply_data, 0);
            return 0;
        }
        case EFFECT_CMD_DISABLE: {
            if (rs != sizeof(int32_t) || reply_data == nullptr) {
                VIPER_LOGE(
                    "EFFECT_CMD_DISABLE called with invalid reply_size = %d, reply_data "
                    "= "
                    "%p, expected reply_size = %zu",
                    rs,
                    reply_data,
                    sizeof(int32_t)
                );
                return -EINVAL;
            }
            analyzer_.Reset();
            enable_ = false;
            SET(int32_t, reply_data, 0);
            return 0;
        }
        case EFFECT_CMD_SET_PARAM: {
            if (cmd_size < sizeof(effect_param_t) || cmd_data == nullptr
                || rs != sizeof(int32_t) || reply_data == nullptr) {
                VIPER_LOGE(
                    "EFFECT_CMD_SET_PARAM called with invalid cmd_size = %d, reply_data "
                    "= "
                    "%p, reply_size = %d, reply_data = %p, expected cmd_size = %zu, "
                    "reply_size = %zu",
                    cmd_size,
                    cmd_data,
                    rs,
                    reply_data,
                    sizeof(effect_param_t),
                    sizeof(int32_t)
                );
                return -EINVAL;
            }
            return HandleSetParam(static_cast<effect_param_t *>(cmd_data), reply_data);
        }
        case EFFECT_CMD_GET_PARAM: {
            if (cmd_size < sizeof(effect_param_t) || cmd_data == nullptr
                || rs < sizeof(effect_param_t) || reply_data == nullptr) {
                VIPER_LOGE(
                    "EFFECT_CMD_GET_PARAM called with invalid cmd_size = %d, reply_data "
                    "= "
                    "%p, reply_size = %d, reply_data = %p, expected cmd_size = %zu, "
                    "reply_size = %zu",
                    cmd_size,
                    cmd_data,
                    rs,
                    reply_data,
                    sizeof(effect_param_t),
                    sizeof(effect_param_t)
                );
                return -EINVAL;
            }
            return HandleGetParam(
                static_cast<effect_param_t *>(cmd_data),
                static_cast<effect_param_t *>(reply_data),
                reply_size
            );
        }
        case EFFECT_CMD_GET_CONFIG: {
            if (rs != sizeof(effect_config_t) || reply_data == nullptr) {
                VIPER_LOGE(
                    "EFFECT_CMD_GET_CONFIG called with invalid reply_size = %d, "
                    "reply_data = %p, expected reply_size = %zu",
                    rs,
                    reply_data,
                    sizeof(effect_config_t)
                );
                return -EINVAL;
            }
            *(effect_config_t *) reply_data = config_;
            return 0;
        }
        default: {
            VIPER_LOGE("HandleCommand called with unknown command: %d", cmd_code);
            return -EINVAL;
        }
    }
}

static audio_buffer_t *GetBuffer(buffer_config_s *config, audio_buffer_t *buffer) {
    if (buffer != nullptr) return buffer;
    if (config->mask & EFFECT_CONFIG_BUFFER) return &config->buffer;
    // EFFECT_CONFIG_PROVIDER not implemented, it's not used by any known effect
    return nullptr;
}

int32_t ViperContext::Process(audio_buffer_t *in_buffer, audio_buffer_t *out_buffer) {
    if (disable_reason_ != DisableReason::NONE) {
        return -EINVAL;
    }

    if (!enable_) {
        return -ENODATA;
    }

    in_buffer = GetBuffer(&config_.input_cfg, in_buffer);
    out_buffer = GetBuffer(&config_.output_cfg, out_buffer);
    if (in_buffer == nullptr || out_buffer == nullptr || in_buffer->raw == nullptr
        || out_buffer->raw == nullptr || in_buffer->frame_count != out_buffer->frame_count
        || in_buffer->frame_count == 0) {
        return -EINVAL;
    }

#if defined(__aarch64__)
    uint64_t orig_fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(orig_fpcr));
    asm volatile("msr fpcr, %0" ::"r"(orig_fpcr | (1 << 24)));
#elif defined(__arm__)
    uint32_t orig_fpscr;
    asm volatile("vmrs %0, fpscr" : "=r"(orig_fpscr));
    asm volatile("vmsr fpscr, %0" ::"r"(orig_fpscr | (1 << 24)));
#endif

    const auto swap = graph_slots_.ConsumePending();
    viper::audio::DspGraph *active_graph = swap.active;
    if (active_graph == nullptr) return -EINVAL;
    if (swap.changed) {
        active_graph->Transition().StartDryToWet();
        if (swap.sample_rate_changed) graph_slots_.ReleasePrevious();
    }
    viper::ViPERParams latest_params{};
    if (parameter_mailbox_.ConsumeLatest(
            applied_parameter_generation_, latest_params
        )) {
        active_graph->Engine().ApplyParams(latest_params);
        if (graph_slots_.Previous() != nullptr) {
            graph_slots_.Previous()->Engine().ApplyParams(latest_params);
        }
    }

    auto now = std::chrono::steady_clock::now();
    if (has_processed_) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - last_process_time_
        )
                           .count();
        if (elapsed > 100) {
            active_graph->Reset();
            analyzer_.Reset();
            active_graph->Transition().StartDryToWet();
            graph_slots_.ReleasePrevious();
        }
    } else {
        active_graph->Reset();
        active_graph->Transition().StartDryToWet();
        graph_slots_.ReleasePrevious();
        has_processed_ = true;
    }
    last_process_time_ = now;

    size_t frame_count = in_buffer->frame_count;
    if (frame_count > buffer_frame_count_) {
        SetDisableReason(
            DisableReason::INVALID_FRAME_COUNT,
            "Process frame count exceeds prepared capacity: " + std::to_string(frame_count)
        );
        return -EINVAL;
    }

    if (!viper::audio::ToFloat(
            buffer_.data(),
            in_buffer->raw,
            frame_count,
            static_cast<audio_format_t>(config_.input_cfg.format)
        )) {
#if defined(__aarch64__)
        asm volatile("msr fpcr, %0" ::"r"(orig_fpcr));
#elif defined(__arm__)
        asm volatile("vmsr fpscr, %0" ::"r"(orig_fpscr));
#endif
        SetDisableReason(DisableReason::INVALID_FORMAT, "Failed to decode input PCM");
        return -EINVAL;
    }
    std::memcpy(
        dry_buffer_.data(),
        buffer_.data(),
        frame_count * viper::audio::kChannelCount * sizeof(float)
    );

    viper::audio::DspGraph *previous_graph = graph_slots_.Previous();
    if (previous_graph != nullptr) {
        std::memcpy(
            previous_buffer_.data(),
            dry_buffer_.data(),
            frame_count * viper::audio::kChannelCount * sizeof(float)
        );
    }

    if (!active_graph->Process(buffer_.data(), frame_count)) {
        SetDisableReason(DisableReason::INVALID_FRAME_COUNT, "DSP graph rejected block");
        return -EINVAL;
    }
    if (previous_graph != nullptr) {
        if (!previous_graph->Process(previous_buffer_.data(), frame_count)) {
            SetDisableReason(
                DisableReason::INVALID_FRAME_COUNT,
                "Previous DSP graph rejected block"
            );
            return -EINVAL;
        }
        active_graph->Transition().Apply(
            buffer_.data(), previous_buffer_.data(), frame_count
        );
        if (!active_graph->Transition().IsActive()) graph_slots_.ReleasePrevious();
    } else {
        active_graph->Transition().Apply(buffer_.data(), dry_buffer_.data(), frame_count);
    }
    analyzer_.Push(buffer_.data(), frame_count);
    const auto compressor_meters = active_graph->Engine().GetCompressorGainReductionDb();
    for (size_t i = 0; i < compressor_meters.size(); ++i) {
        analyzer_.SetMeter(i, compressor_meters[i]);
    }

    const bool accumulate =
        config_.output_cfg.access_mode == EFFECT_BUFFER_ACCESS_ACCUMULATE;
    if (!viper::audio::FromFloat(
            out_buffer->raw,
            buffer_.data(),
            frame_count,
            static_cast<audio_format_t>(config_.output_cfg.format),
            accumulate
        )) {
#if defined(__aarch64__)
        asm volatile("msr fpcr, %0" ::"r"(orig_fpcr));
#elif defined(__arm__)
        asm volatile("vmsr fpscr, %0" ::"r"(orig_fpscr));
#endif
        SetDisableReason(DisableReason::INVALID_FORMAT, "Failed to encode output PCM");
        return -EINVAL;
    }

#if defined(__aarch64__)
    asm volatile("msr fpcr, %0" ::"r"(orig_fpcr));
#elif defined(__arm__)
    asm volatile("vmsr fpscr, %0" ::"r"(orig_fpscr));
#endif

    return 0;
}

void ViperContext::SetDisableReason(const DisableReason reason) {
    SetDisableReason(reason, "");
}

void ViperContext::SetDisableReason(const DisableReason reason, std::string message) {
    this->disable_reason_ = reason;
    this->disable_reason_message_ = std::move(message);
}
