#pragma once

#include "DspGraphSlots.h"
#include "DspResources.h"
#include "IemContext.h"
#include "ParameterMailbox.h"
#include "essential.h"
#include "viper/ParameterSnapshot.h"
#include "viper/effects/AudioAnalyzer.h"
#include <chrono>
#include <string>
#include <vector>

class ViperContext {
public:
    enum class DisableReason : int32_t {
        UNKNOWN = -1,
        NONE = 0,
        INVALID_FRAME_COUNT,
        INVALID_SAMPLING_RATE,
        INVALID_CHANNEL_COUNT,
        INVALID_FORMAT,
    };

    ViperContext();

    int32_t HandleCommand(
        uint32_t cmd_code,
        uint32_t cmd_size,
        void *cmd_data,
        uint32_t *reply_size,
        void *reply_data
    );
    int32_t Process(audio_buffer_t *in_buffer, audio_buffer_t *out_buffer);

private:
    effect_config_t config_;
    DisableReason disable_reason_;
    std::string disable_reason_message_;

    // Processing buffer
    std::vector<float> buffer_;
    std::vector<float> dry_buffer_;
    std::vector<float> previous_buffer_;
    size_t buffer_frame_count_;

    // Viper
    bool enable_;
    viper::audio::DspGraphSlots graph_slots_;
    viper::audio::DspResources resources_;
    viper::audio::ParameterMailbox parameter_mailbox_;
    viper::audio::IemContext iem_context_;
    viper::ViPERParams parameter_snapshot_{};
    uint64_t applied_parameter_generation_ = 0;
    viper::AudioAnalyzer analyzer_;
    uint64_t graph_generation_ = 0;
    uint64_t last_streaming_frames_ = 0;

    // Stream discontinuity detection
    std::chrono::steady_clock::time_point last_process_time_;
    bool has_processed_;

    static void CopyBufferConfig(buffer_config_t *dest, buffer_config_t *src);
    void HandleSetConfig(effect_config_t *new_config);

    int32_t HandleSetParam(effect_param_t *cmd_param, void *reply_data);
    int32_t HandleGetParam(
        effect_param_t *cmd_param, effect_param_t *reply_param, uint32_t *reply_size
    );

    void DispatchRawParam(
        int param,
        int val1,
        int val2,
        int val3,
        uint32_t arr_size,
        signed char *arr
    );
    void ResetGraphs() noexcept;

    void SetDisableReason(DisableReason reason);
    void SetDisableReason(DisableReason reason, std::string message);
};
