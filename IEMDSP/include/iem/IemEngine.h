#pragma once

#include "iem/AlignedPlanarBuffer.h"
#include "iem/IemParams.h"
#include "iem/IemPipeline.h"
#include "iem/IemTelemetry.h"
#include "iem/PlanarBlockScheduler.h"
#include "iem/StreamingResampler.h"

#include <cstddef>
#include <cstdint>

namespace iem {

struct IemEngineConfig {
    uint32_t host_sample_rate = 0;
    std::size_t max_host_block_frames = 0;
    uint32_t internal_sample_rate = 96000;
    std::size_t internal_block_frames = 256;
    uint32_t max_channels = 16;
};

class IemEngine final {
public:
    static constexpr uint32_t kMinHostSampleRate = 8000;
    static constexpr uint32_t kMaxHostSampleRate = 384000;
    static constexpr std::size_t kMaxHostBlockFrames = 8192;

    IemEngine() = default;
    IemEngine(IemEngine &&) noexcept = default;
    IemEngine &operator=(IemEngine &&) noexcept = default;
    IemEngine(const IemEngine &) = delete;
    IemEngine &operator=(const IemEngine &) = delete;

    bool Prepare(const IemEngineConfig &config, const IemParams &params) noexcept;
    void ApplyParams(const IemParams &params) noexcept;
    void SetFreeze(bool freeze) noexcept { pipeline_.SetFreeze(freeze); }
    void ResetAngles() noexcept { pipeline_.ResetAngles(); }
    bool Process(float *stereo_interleaved, std::size_t host_frames) noexcept;
    void Reset() noexcept;

    uint32_t LatencyFrames() const noexcept { return latency_frames_; }
    bool IsPrepared() const noexcept { return prepared_; }
    const IemParams &Params() const noexcept { return params_; }
    uint32_t ActiveGrainCount() const noexcept { return pipeline_.ActiveGrainCount(); }
    bool IsFrozen() const noexcept { return pipeline_.IsFrozen(); }
    uint64_t GrainPoolExhaustionCount() const noexcept {
        return pipeline_.GrainPoolExhaustionCount();
    }
    float LimiterGainReductionDb() const noexcept {
        return pipeline_.LimiterGainReductionDb();
    }
    IemResourceError PipelineError() const noexcept { return pipeline_.Error(); }
    bool ReadTelemetry(IemTelemetrySnapshot &snapshot) const noexcept {
        return telemetry_.Read(snapshot);
    }

private:
    static constexpr std::size_t kEnableRampFrames = 256;

    bool PrepareBuffers() noexcept;
    void ResetPipeline() noexcept;
    void UpdateEnableRamp() noexcept;

    IemEngineConfig config_{};
    IemParams params_{};
    StreamingResampler input_resampler_;
    StreamingResampler output_resampler_;
    PlanarBlockScheduler input_scheduler_;
    PlanarBlockScheduler output_fifo_;
    IemPipeline pipeline_;
    IemTelemetryPublisher telemetry_;
    AlignedPlanarBuffer host_input_;
    AlignedPlanarBuffer host_processed_;
    AlignedPlanarBuffer internal_resampled_;
    AlignedPlanarBuffer internal_processed_;
    AlignedPlanarBuffer host_resampled_;
    std::size_t internal_scratch_frames_ = 0;
    std::size_t host_resample_scratch_frames_ = 0;
    uint32_t latency_frames_ = 0;
    uint32_t transport_latency_frames_ = 0;
    uint64_t emitted_host_frames_ = 0;
    float enable_mix_ = 0.0F;
    float target_enable_mix_ = 0.0F;
    bool active_ = false;
    bool prepared_ = false;
};

} // namespace iem
