#pragma once

#include "iem/AlignedPlanarBuffer.h"
#include "iem/GranularEncoder.h"
#include "iem/HaloEncoder.h"
#include "iem/HeadphoneEq.h"
#include "iem/LinkedLookaheadLimiter.h"
#include "iem/LinearSmoother.h"
#include "iem/MultiEncoder.h"
#include "iem/SceneRotator.h"
#include "iem/SimpleDecoder.h"
#include "iem/StereoEncoder.h"

#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

namespace iem {

struct LatencyProfileConfig {
    uint32_t partition_frames;
    uint32_t scheduler_waterline_blocks;
    uint32_t maximum_latency_ms;
};

constexpr LatencyProfileConfig kLatencyProfiles[3]{
    {64, 1, 10},
    {128, 2, 20},
    {256, 4, 40},
};

class IemPipeline {
public:
    IemPipeline() = default;
    IemPipeline(IemPipeline &&) noexcept = default;
    IemPipeline &operator=(IemPipeline &&) noexcept = default;
    IemPipeline(const IemPipeline &) = delete;
    IemPipeline &operator=(const IemPipeline &) = delete;

    bool Prepare(const IemParams &params, std::size_t max_frames) noexcept;
    void ApplyParams(const IemParams &params) noexcept;
    bool Process(const float *const stereo[2], float *const output[2],
        std::size_t frames) noexcept;
    void SetFreeze(bool freeze) noexcept;
    void ResetAngles() noexcept;
    void Reset() noexcept;

    uint32_t LatencyFrames() const noexcept { return total_latency_frames_; }
    uint32_t WetLatencyFrames() const noexcept { return wet_latency_frames_; }
    uint32_t ActiveGrainCount() const noexcept;
    uint64_t GrainPoolExhaustionCount() const noexcept;
    bool IsFrozen() const noexcept;
    float LimiterGainReductionDb() const noexcept {
        return limiter_.GainReductionDb();
    }
    IemResourceError Error() const noexcept { return error_; }
    const LatencyProfileConfig &ProfileConfig() const noexcept {
        return profile_config_;
    }

private:
    using EncoderVariant = std::variant<std::monostate,
        StereoEncoder, MultiEncoder, GranularEncoder, HaloEncoder>;

    IemEncoder *Encoder() noexcept;
    const IemEncoder *Encoder() const noexcept;
    HaloEncoder *Halo() noexcept;
    const HaloEncoder *Halo() const noexcept;
    bool PrepareEncoder(const IemParams &params, std::size_t max_frames) noexcept;
    void ClearRuntimeBuffers() noexcept;

    IemParams params_{};
    EncoderVariant encoder_{};
    SceneRotator rotator_{};
    SimpleDecoder simple_decoder_{};
    Ku100Decoder decoder_{};
    HeadphoneEq headphone_eq_{};
    LinkedLookaheadLimiter limiter_{};
    AlignedPlanarBuffer encoded_{};
    AlignedPlanarBuffer halo_bed_{};
    AlignedPlanarBuffer rotated_{};
    AlignedPlanarBuffer decoded_{};
    AlignedPlanarBuffer equalized_{};
    AlignedPlanarBuffer delayed_dry_{};
    AlignedPlanarBuffer mix_{};
    std::vector<float> dry_delay_{};
    LinearSmoother wet_smoother_{};
    LinearSmoother gain_smoother_{};
    LatencyProfileConfig profile_config_ = kLatencyProfiles[1];
    std::size_t max_frames_ = 0;
    uint32_t wet_latency_frames_ = 0;
    uint32_t total_latency_frames_ = 0;
    uint32_t dry_delay_index_ = 0;
    IemResourceError error_ = IemResourceError::NONE;
    bool prepared_ = false;
};

} // namespace iem
