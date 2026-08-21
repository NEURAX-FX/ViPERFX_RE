#pragma once

#include "DriverDaemonBridge.h"
#include "DspGraphSlots.h"
#include "DspResources.h"
#include "IemContext.h"
#include "ParameterMailbox.h"
#include "SnapshotApplyController.h"
#include "essential.h"
#include "viper/ParameterSnapshot.h"
#include "viper/effects/AudioAnalyzer.h"
#include <chrono>
#include <cstdint>
#include <mutex>
#include <span>
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
    ~ViperContext();

    // Called once from the effect library create path, off the audio thread.
    void AttachDaemonBridge(uint32_t audio_session_id, uint32_t io_id);

    // Daemon snapshot apply path. Runs on the daemon bridge thread, which is not
    // an AudioFlinger command thread, so these serialize on control_mutex_
    // against each other and against HandleCommand().
    void SetDaemonRoute(std::string device_key_hash);
    bool BeginSnapshot(
        const viper::audio::SnapshotMetadata &metadata,
        viper::audio::ApplyError *error
    );
    bool AppendSnapshot(
        uint32_t offset,
        std::span<const uint8_t> chunk,
        viper::audio::ApplyError *error
    );
    bool CommitSnapshot(
        viper::audio::ApplyResult *result,
        viper::audio::ApplyError *error,
        std::string *message
    );
    void AbortSnapshot(viper::audio::ApplyError reason);

    // Translates one decoded daemon command into the Begin/Append/Commit/Abort
    // calls above and reports the outcome for the ACK/NACK frame. Runs on the
    // daemon bridge thread.
    viper::daemon::DriverDaemonBridge::SnapshotAck HandleSnapshotCommand(
        viper::daemon::SnapshotCommandType type,
        std::span<const uint8_t> payload
    );

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

    // Daemon observation identity; 0 means the bridge is not attached.
    uint64_t daemon_context_instance_id_ = 0;
    viper::audio::SnapshotApplyController snapshot_apply_;

    // Serializes control-thread state: AudioFlinger command threads and the
    // daemon bridge thread both mutate parameters, resources and graph slots.
    // NEVER taken by Process(): the audio thread must not block on the daemon.
    std::mutex control_mutex_;

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

    // Control-thread only; never called from Process().
    void PublishDaemonGenerations();
    void PublishDaemonTelemetry();

    // Applies a decoded daemon snapshot. Control thread only: it replays raw
    // params, prepares a pending graph, and publishes atomically.
    bool CommitDaemonSnapshot(
        const viper::audio::SnapshotApplyController::CommitRequest &request,
        uint64_t *resource_generation,
        uint64_t *graph_generation,
        std::string *error
    );

    void SetDisableReason(DisableReason reason);
    void SetDisableReason(DisableReason reason, std::string message);
};
