#include "IemContext.h"

#include "AudioFormat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace viper::audio {

bool IemContext::Prepare(
    uint32_t host_sample_rate,
    std::size_t max_block_frames
) {
    if (!IsSupportedSampleRate(host_sample_rate)
        || max_block_frames == 0 || max_block_frames > kMaxBlockFrames
        || max_block_frames > std::numeric_limits<std::size_t>::max() / kChannelCount) {
        return false;
    }

    std::vector<float> replacement_dry(
        max_block_frames * kChannelCount, 0.0F
    );
    std::vector<float> replacement_previous(
        max_block_frames * kChannelCount, 0.0F
    );
    const IemGraphConfig graph_config{
        host_sample_rate,
        max_block_frames,
        graph_generation_ + 1,
    };
    const bool graph_prepared = graph_slots_.Active() == nullptr
        ? graph_slots_.PrepareInitial(graph_config, parameter_snapshot_, resources_)
        : graph_slots_.PreparePending(graph_config, parameter_snapshot_, resources_);
    if (!graph_prepared) return false;

    dry_buffer_.swap(replacement_dry);
    previous_buffer_.swap(replacement_previous);
    host_sample_rate_ = host_sample_rate;
    max_block_frames_ = max_block_frames;
    graph_generation_ = graph_config.generation;
    structural_dirty_ = false;
    last_preparation_result_.store(
        static_cast<uint32_t>(iem::IemPreparationResult::SUCCESS),
        std::memory_order_release
    );
    prepared_ = true;
    if (graph_slots_.Pending() == nullptr && parameter_snapshot_.enable) {
        graph_slots_.Active()->Transition().StartDryToWet();
    }
    return true;
}

bool IemContext::DispatchRawParam(
    int param,
    int val1,
    int val2,
    int val3
) noexcept {
    const iem::IemParams previous_snapshot = parameter_snapshot_;
    const iem::ParamUpdate result = iem::UpdateIemParameterSnapshot(
        parameter_snapshot_, param, val1, val2, val3
    );
    if (result == iem::ParamUpdate::NOT_IEM) return false;
    if (result == iem::ParamUpdate::INVALID) {
        last_preparation_result_.store(
            static_cast<uint32_t>(iem::IemPreparationResult::INVALID_PARAMETER),
            std::memory_order_release
        );
        return true;
    }
    if (result == iem::ParamUpdate::UPDATED) {
        const bool structural_change = iem::HasStructuralDifference(
            previous_snapshot, parameter_snapshot_);
        if (structural_change) structural_dirty_ = true;
        parameter_mailbox_.Publish(parameter_snapshot_);
        if (parameter_snapshot_.enable && structural_dirty_) {
            TryPrepareDesiredGraph();
        } else if (structural_change) {
            last_preparation_result_.store(
                static_cast<uint32_t>(iem::IemPreparationResult::DEFERRED),
                std::memory_order_release
            );
        }
        return true;
    }

    if (param == iem::kCommandGranularFreeze) {
        std::array<IemGraph *, 3> graphs{
            graph_slots_.Active(), graph_slots_.Pending(), graph_slots_.Previous()
        };
        for (std::size_t index = 0; index < graphs.size(); ++index) {
            if (graphs[index] == nullptr) continue;
            bool duplicate = false;
            for (std::size_t previous = 0; previous < index; ++previous) {
                if (graphs[previous] == graphs[index]) duplicate = true;
            }
            if (!duplicate) graphs[index]->Engine().SetFreeze(val1 != 0);
        }
        return true;
    }
    if (param == iem::kCommandResetRotation) {
        parameter_snapshot_.rotation.yaw_centidegrees = 0;
        parameter_snapshot_.rotation.pitch_centidegrees = 0;
        parameter_snapshot_.rotation.roll_centidegrees = 0;
        parameter_mailbox_.Publish(parameter_snapshot_);
        std::array<IemGraph *, 3> graphs{
            graph_slots_.Active(), graph_slots_.Pending(), graph_slots_.Previous()
        };
        for (IemGraph *graph : graphs) {
            if (graph != nullptr) graph->Engine().ResetAngles();
        }
        return true;
    }
    if (param == iem::kCommandResetIemRuntime) {
        Reset();
        return true;
    }
    if (!resources_.CaptureRaw(param, val1)) return true;
    structural_dirty_ = true;
    if (!prepared_ || !parameter_snapshot_.enable) {
        last_preparation_result_.store(
            static_cast<uint32_t>(iem::IemPreparationResult::DEFERRED),
            std::memory_order_release
        );
        return true;
    }
    TryPrepareDesiredGraph();
    return true;
}

bool IemContext::TryPrepareDesiredGraph() noexcept {
    if (!prepared_ || !parameter_snapshot_.enable) return false;
    const auto matches_desired = [this](const IemGraph *graph) {
        return graph != nullptr
            && !iem::HasStructuralDifference(
                graph->Engine().Params(), parameter_snapshot_)
            && graph->ResourceGeneration() == resources_.Generation();
    };
    if (matches_desired(graph_slots_.Pending())) {
        structural_dirty_ = false;
        last_preparation_result_.store(
            static_cast<uint32_t>(iem::IemPreparationResult::SUCCESS),
            std::memory_order_release
        );
        return true;
    }
    if (graph_slots_.Pending() != nullptr && !graph_slots_.CancelPending()) {
        last_preparation_result_.store(
            static_cast<uint32_t>(iem::IemPreparationResult::DEFERRED),
            std::memory_order_release
        );
        return false;
    }
    if (matches_desired(graph_slots_.Active())) {
        structural_dirty_ = false;
        last_preparation_result_.store(
            static_cast<uint32_t>(iem::IemPreparationResult::SUCCESS),
            std::memory_order_release
        );
        return true;
    }
    const IemGraphConfig graph_config{
        host_sample_rate_,
        max_block_frames_,
        graph_generation_ + 1,
    };
    if (graph_slots_.PreparePending(
            graph_config, parameter_snapshot_, resources_
        )) {
        graph_generation_ = graph_config.generation;
        structural_dirty_ = false;
        last_preparation_result_.store(
            static_cast<uint32_t>(iem::IemPreparationResult::SUCCESS),
            std::memory_order_release
        );
        return true;
    }
    last_preparation_result_.store(
        static_cast<uint32_t>(iem::IemPreparationResult::FAILED),
        std::memory_order_release
    );
    return false;
}

bool IemContext::Process(
    float *post_viper_interleaved,
    std::size_t frame_count
) noexcept {
    if (!prepared_ || post_viper_interleaved == nullptr
        || frame_count == 0 || frame_count > max_block_frames_) {
        return false;
    }

    const std::size_t sample_count = frame_count * kChannelCount;
    const std::size_t byte_count = sample_count * sizeof(float);
    std::memcpy(dry_buffer_.data(), post_viper_interleaved, byte_count);
    for (std::size_t sample = 0; sample < sample_count; ++sample) {
        if (!std::isfinite(dry_buffer_[sample])) dry_buffer_[sample] = 0.0F;
    }

    const IemGraphSwapResult swap = graph_slots_.ConsumePending();
    IemGraph *active_graph = swap.active;
    if (active_graph == nullptr) return false;
    if (swap.changed) {
        const bool transition_needed = active_graph->Engine().Params().enable
            || (swap.previous != nullptr && swap.previous->Engine().Params().enable);
        if (transition_needed) {
            const bool latency_changed = swap.previous != nullptr
                && active_graph->Engine().LatencyFrames()
                    != swap.previous->Engine().LatencyFrames();
            if (latency_changed) active_graph->Transition().StartFadeThroughSilence();
            else active_graph->Transition().StartDryToWet();
        }
        if (swap.sample_rate_changed || !transition_needed) {
            graph_slots_.ReleasePrevious();
        }
    }

    iem::IemParams latest{};
    if (parameter_mailbox_.ConsumeLatest(
            applied_parameter_generation_, latest
        )) {
        active_graph->ApplyParams(latest);
        if (graph_slots_.Previous() != nullptr) {
            graph_slots_.Previous()->ApplyParams(latest);
        }
    }

    IemGraph *previous_graph = graph_slots_.Previous();
    if (previous_graph != nullptr) {
        std::memcpy(previous_buffer_.data(), dry_buffer_.data(), byte_count);
    }

    if (!active_graph->Process(post_viper_interleaved, frame_count)) {
        std::memcpy(post_viper_interleaved, dry_buffer_.data(), byte_count);
        graph_slots_.ReleasePrevious();
        active_graph->Reset();
        return false;
    }
    if (previous_graph != nullptr) {
        if (!previous_graph->Process(previous_buffer_.data(), frame_count)) {
            std::memcpy(post_viper_interleaved, dry_buffer_.data(), byte_count);
            graph_slots_.ReleasePrevious();
            previous_graph->Reset();
            return false;
        }
        active_graph->Transition().Apply(
            post_viper_interleaved, previous_buffer_.data(), frame_count
        );
        if (!active_graph->Transition().IsActive()) {
            graph_slots_.ReleasePrevious();
        }
    } else if (active_graph->Engine().Params().enable
               && active_graph->Transition().IsActive()) {
        active_graph->Transition().Apply(
            post_viper_interleaved, dry_buffer_.data(), frame_count
        );
    }
    return true;
}

void IemContext::Reset() noexcept {
    std::array<IemGraph *, 3> graphs{
        graph_slots_.Active(), graph_slots_.Pending(), graph_slots_.Previous()
    };
    for (std::size_t index = 0; index < graphs.size(); ++index) {
        if (graphs[index] == nullptr) continue;
        bool duplicate = false;
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (graphs[previous] == graphs[index]) duplicate = true;
        }
        if (!duplicate) {
            graphs[index]->Reset();
            graphs[index]->Transition().Reset();
        }
    }
    graph_slots_.ReleasePrevious();
    if (graph_slots_.Active() != nullptr
        && graph_slots_.Active()->Engine().Params().enable) {
        graph_slots_.Active()->Transition().StartDryToWet();
    }
    std::fill(dry_buffer_.begin(), dry_buffer_.end(), 0.0F);
    std::fill(previous_buffer_.begin(), previous_buffer_.end(), 0.0F);
}

bool IemContext::ReadTelemetry(
    iem::IemTelemetrySnapshot &snapshot
) const noexcept {
    const IemGraph *active = graph_slots_.Active();
    if (active == nullptr || !active->Engine().ReadTelemetry(snapshot)) return false;
    snapshot.graph_generation = active->Config().generation;
    snapshot.preparation_result = LastPreparationResult();
    return true;
}

} // namespace viper::audio
