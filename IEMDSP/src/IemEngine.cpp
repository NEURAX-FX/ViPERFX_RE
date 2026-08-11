#include "iem/IemEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace iem {
namespace {

bool AddWithoutOverflow(
    std::size_t left,
    std::size_t right,
    std::size_t &result
) noexcept {
    if (left > std::numeric_limits<std::size_t>::max() - right) return false;
    result = left + right;
    return true;
}

} // namespace

bool IemEngine::Prepare(
    const IemEngineConfig &config,
    const IemParams &params
) noexcept {
    if (config.host_sample_rate < kMinHostSampleRate
        || config.host_sample_rate > kMaxHostSampleRate
        || config.max_host_block_frames == 0
        || config.max_host_block_frames > kMaxHostBlockFrames
        || config.internal_sample_rate != 96000
        || config.internal_block_frames != PlanarBlockScheduler::kBlockFrames
        || config.max_channels != AlignedPlanarBuffer::kMaxChannels) {
        return false;
    }

    IemEngine replacement;
    replacement.config_ = config;
    replacement.params_ = params;
    if (!replacement.pipeline_.Prepare(params, config.internal_block_frames)) return false;
    if (!replacement.PrepareBuffers()) return false;
    replacement.active_ = params.enable;
    replacement.enable_mix_ = params.enable ? 1.0F : 0.0F;
    replacement.target_enable_mix_ = replacement.enable_mix_;
    replacement.prepared_ = true;
    replacement.ResetPipeline();
    replacement.telemetry_.Configure(
        config.host_sample_rate, config.internal_sample_rate
    );

    *this = std::move(replacement);
    return true;
}

void IemEngine::ApplyParams(const IemParams &params) noexcept {
    const bool was_enabled = params_.enable;
    params_ = params;
    pipeline_.ApplyParams(params);

    if (params.enable && !was_enabled) {
        ResetPipeline();
        active_ = true;
        enable_mix_ = 0.0F;
        target_enable_mix_ = 1.0F;
    } else if (!params.enable && was_enabled && active_) {
        target_enable_mix_ = 0.0F;
    } else if (params.enable) {
        active_ = true;
        target_enable_mix_ = 1.0F;
    }
}

bool IemEngine::Process(
    float *stereo_interleaved,
    std::size_t host_frames
) noexcept {
    if (!prepared_ || stereo_interleaved == nullptr
        || host_frames == 0 || host_frames > config_.max_host_block_frames) {
        telemetry_.RecordFailure(
            prepared_ ? IemBypassReason::INVALID_BLOCK
                      : IemBypassReason::NOT_PREPARED
        );
        return false;
    }
    const auto process_start = std::chrono::steady_clock::now();
    const auto finish = [this, process_start, host_frames](
                            bool success,
                            IemBypassReason reason
                        ) noexcept {
        const uint64_t process_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - process_start
            ).count()
        );
        const uint64_t budget_ns = static_cast<uint64_t>(host_frames)
            * 1000000000ULL / config_.host_sample_rate;
        telemetry_.RecordSpatialState(
            static_cast<uint32_t>(params_.encoder_mode),
            params_.order,
            pipeline_.ActiveGrainCount(),
            pipeline_.GrainPoolExhaustionCount(),
            static_cast<uint32_t>(pipeline_.Error()),
            pipeline_.LimiterGainReductionDb()
        );
        telemetry_.RecordBlock(
            host_frames,
            process_ns,
            budget_ns,
            latency_frames_,
            params_.enable,
            reason
        );
        if (!success) telemetry_.RecordFailure(reason);
        return success;
    };
    if (!active_ && !params_.enable) {
        return finish(true, IemBypassReason::DISABLED);
    }

    float *input_left = host_input_.ChannelData(0);
    float *input_right = host_input_.ChannelData(1);
    for (std::size_t frame = 0; frame < host_frames; ++frame) {
        const float left = stereo_interleaved[frame * 2];
        const float right = stereo_interleaved[frame * 2 + 1];
        if (!std::isfinite(left) || !std::isfinite(right)) {
            return finish(false, IemBypassReason::NON_FINITE);
        }
        input_left[frame] = left;
        input_right[frame] = right;
    }

    const float *host_input_ptrs[2]{input_left, input_right};
    float *internal_ptrs[2]{
        internal_resampled_.ChannelData(0),
        internal_resampled_.ChannelData(1),
    };
    const std::size_t internal_frames = input_resampler_.Process(
        host_input_ptrs,
        host_frames,
        internal_ptrs,
        internal_scratch_frames_
    );
    if (input_resampler_.Failed()) {
        return finish(false, IemBypassReason::RESAMPLER_FAILURE);
    }
    const float *internal_const_ptrs[2]{internal_ptrs[0], internal_ptrs[1]};
    if (internal_frames != 0
        && !input_scheduler_.Push(internal_const_ptrs, internal_frames)) {
        telemetry_.RecordQueueOverflow(true);
        return finish(false, IemBypassReason::INVALID_BLOCK);
    }

    while (input_scheduler_.HasBlock()) {
        const float *block_ptrs[2]{
            input_scheduler_.ChannelBlock(0),
            input_scheduler_.ChannelBlock(1),
        };
        float *processed_internal_ptrs[2]{
            internal_processed_.ChannelData(0),
            internal_processed_.ChannelData(1),
        };
        if (!pipeline_.Process(
                block_ptrs,
                processed_internal_ptrs,
                PlanarBlockScheduler::kBlockFrames
            )) {
            return finish(false, IemBypassReason::PROCESS_FAILURE);
        }
        const float *processed_internal_const_ptrs[2]{
            processed_internal_ptrs[0], processed_internal_ptrs[1]
        };
        float *host_resampled_ptrs[2]{
            host_resampled_.ChannelData(0),
            host_resampled_.ChannelData(1),
        };
        const std::size_t produced = output_resampler_.Process(
            processed_internal_const_ptrs,
            PlanarBlockScheduler::kBlockFrames,
            host_resampled_ptrs,
            host_resample_scratch_frames_
        );
        if (output_resampler_.Failed()) {
            return finish(false, IemBypassReason::RESAMPLER_FAILURE);
        }
        const float *host_resampled_const_ptrs[2]{
            host_resampled_ptrs[0], host_resampled_ptrs[1]
        };
        if (produced != 0
            && !output_fifo_.Push(host_resampled_const_ptrs, produced)) {
            telemetry_.RecordQueueOverflow(false);
            return finish(false, IemBypassReason::INVALID_BLOCK);
        }
        input_scheduler_.ConsumeBlock();
    }

    float *processed_left = host_processed_.ChannelData(0);
    float *processed_right = host_processed_.ChannelData(1);
    std::memset(processed_left, 0, host_frames * sizeof(float));
    std::memset(processed_right, 0, host_frames * sizeof(float));

    const std::size_t initial_zeros = emitted_host_frames_ < transport_latency_frames_
        ? std::min<std::size_t>(
              host_frames,
              transport_latency_frames_ - static_cast<std::size_t>(emitted_host_frames_)
          )
        : 0;
    const std::size_t required_processed = host_frames - initial_zeros;
    if (required_processed > output_fifo_.AvailableFrames()) {
        return finish(false, IemBypassReason::OUTPUT_UNDERFLOW);
    }
    if (required_processed != 0) {
        float *processed_ptrs[2]{
            processed_left + initial_zeros,
            processed_right + initial_zeros,
        };
        if (!output_fifo_.Pop(processed_ptrs, required_processed)) {
            return finish(false, IemBypassReason::OUTPUT_UNDERFLOW);
        }
    }

    for (std::size_t frame = 0; frame < host_frames; ++frame) {
        if (emitted_host_frames_ + frame >= latency_frames_) UpdateEnableRamp();
        const float mix = enable_mix_;
        stereo_interleaved[frame * 2] = input_left[frame] * (1.0F - mix)
            + processed_left[frame] * mix;
        stereo_interleaved[frame * 2 + 1] =
            input_right[frame] * (1.0F - mix) + processed_right[frame] * mix;
    }
    emitted_host_frames_ += host_frames;

    if (!params_.enable && enable_mix_ <= 0.0F) {
        active_ = false;
        ResetPipeline();
    }
    return finish(
        true,
        params_.enable ? IemBypassReason::NONE : IemBypassReason::DISABLED
    );
}

void IemEngine::Reset() noexcept {
    if (!prepared_) return;
    active_ = params_.enable;
    enable_mix_ = params_.enable ? 1.0F : 0.0F;
    target_enable_mix_ = enable_mix_;
    ResetPipeline();
}

bool IemEngine::PrepareBuffers() noexcept {
    if (!input_resampler_.Prepare(
            config_.host_sample_rate,
            config_.internal_sample_rate,
            2,
            config_.max_host_block_frames
        )) {
        return false;
    }
    internal_scratch_frames_ =
        input_resampler_.MaxOutputFrames(config_.max_host_block_frames);
    if (internal_scratch_frames_ == 0
        || internal_scratch_frames_ == std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    if (!output_resampler_.Prepare(
            config_.internal_sample_rate,
            config_.host_sample_rate,
            2,
            PlanarBlockScheduler::kBlockFrames
        )) {
        return false;
    }
    host_resample_scratch_frames_ =
        output_resampler_.MaxOutputFrames(PlanarBlockScheduler::kBlockFrames);
    if (host_resample_scratch_frames_ == 0
        || host_resample_scratch_frames_ == std::numeric_limits<std::size_t>::max()) {
        return false;
    }

    const uint64_t transport_internal_latency =
        PlanarBlockScheduler::kBlockFrames + output_resampler_.LatencyInputFrames();
    const uint64_t converted_transport_latency =
        (transport_internal_latency * config_.host_sample_rate
         + config_.internal_sample_rate - 1)
        / config_.internal_sample_rate;
    const uint64_t transport_latency = input_resampler_.LatencyInputFrames()
        + converted_transport_latency + 4U;
    const uint64_t converted_pipeline_latency =
        (static_cast<uint64_t>(pipeline_.LatencyFrames()) * config_.host_sample_rate
            + config_.internal_sample_rate - 1)
        / config_.internal_sample_rate;
    const uint64_t latency = transport_latency + converted_pipeline_latency;
    if (latency > std::numeric_limits<uint32_t>::max()) return false;
    transport_latency_frames_ = static_cast<uint32_t>(transport_latency);
    latency_frames_ = static_cast<uint32_t>(latency);

    std::size_t input_capacity = 0;
    if (!AddWithoutOverflow(
            internal_scratch_frames_,
            PlanarBlockScheduler::kBlockFrames * 2,
            input_capacity
        )) {
        return false;
    }
    std::size_t output_capacity = 0;
    if (!AddWithoutOverflow(
            config_.max_host_block_frames * 2,
            host_resample_scratch_frames_ * 4,
            output_capacity
        ) || !AddWithoutOverflow(output_capacity, latency_frames_, output_capacity)) {
        return false;
    }
    return host_input_.Prepare(2, config_.max_host_block_frames)
        && host_processed_.Prepare(2, config_.max_host_block_frames)
        && internal_resampled_.Prepare(2, internal_scratch_frames_)
        && internal_processed_.Prepare(2, PlanarBlockScheduler::kBlockFrames)
        && host_resampled_.Prepare(2, host_resample_scratch_frames_)
        && input_scheduler_.Prepare(2, input_capacity)
        && output_fifo_.Prepare(2, output_capacity);
}

void IemEngine::ResetPipeline() noexcept {
    input_resampler_.Reset();
    output_resampler_.Reset();
    input_scheduler_.Reset();
    output_fifo_.Reset();
    pipeline_.Reset();
    host_input_.Clear();
    host_processed_.Clear();
    internal_resampled_.Clear();
    internal_processed_.Clear();
    host_resampled_.Clear();
    emitted_host_frames_ = 0;
}

void IemEngine::UpdateEnableRamp() noexcept {
    if (enable_mix_ == target_enable_mix_) return;
    const float step = 1.0F / static_cast<float>(kEnableRampFrames);
    if (enable_mix_ < target_enable_mix_) {
        enable_mix_ = std::min(target_enable_mix_, enable_mix_ + step);
    } else {
        enable_mix_ = std::max(target_enable_mix_, enable_mix_ - step);
    }
}

} // namespace iem
