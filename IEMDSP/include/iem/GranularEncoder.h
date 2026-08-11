#pragma once

#include "iem/FixedRandom.h"
#include "iem/Grain.h"
#include "iem/IemEncoder.h"
#include "iem/Quaternion.h"

#include <array>
#include <vector>

namespace iem {

class GranularEncoder final : public IemEncoder {
public:
    static constexpr std::size_t kHistorySeconds = 8;
    static constexpr std::size_t kMaxGrains = 512;

    GranularEncoder() = default;
    GranularEncoder(const GranularEncoder &) = delete;
    GranularEncoder &operator=(const GranularEncoder &) = delete;
    GranularEncoder(GranularEncoder &&) = default;
    GranularEncoder &operator=(GranularEncoder &&) = default;

    bool Prepare(const EncoderConfig &config) override;
    void ApplyParams(const IemParams &params) noexcept override;
    void Reset() noexcept override;
    bool Process(
        const float *const stereo[2],
        float *const ambisonics[kMaxAmbisonicsChannels],
        std::size_t frames
    ) noexcept override;

    void SetFreeze(bool freeze) noexcept;
    bool IsFrozen() const noexcept { return frozen_; }
    uint32_t ActiveGrainCount() const noexcept {
        return static_cast<uint32_t>(active_count_);
    }
    uint64_t PoolExhaustionCount() const noexcept { return pool_exhaustion_count_; }
    uint32_t WriteHeadForTest() const noexcept { return write_head_; }
    uint32_t ValidHistoryFramesForTest() const noexcept { return valid_history_frames_; }
    Vec3 LastSpawnedDirectionForTest() const noexcept { return last_spawned_direction_; }

private:
    struct CenterControls {
        float azimuth = 0.0F;
        float elevation = 0.0F;
        float roll = 0.0F;
    };

    void ClampParams() noexcept;
    void EvaluateCenter() noexcept;
    Vec3 RandomDirection() noexcept;
    float SymmetricSpreadSample() noexcept;
    uint32_t RandomDeltaSamples() noexcept;
    bool SpawnGrain() noexcept;
    float MeanWindowPower() const noexcept;
    float GrainGain() const noexcept;
    static float InterpolateAngle(float start, float end, float amount) noexcept;

    EncoderConfig config_{};
    GranularParams params_{};
    CenterControls current_center_{};
    CenterControls target_center_{};
    std::array<float, kMaxAmbisonicsChannels> current_center_weights_{};
    std::array<float, kMaxAmbisonicsChannels> target_center_weights_{};
    std::vector<float> history_{};
    std::array<Grain, kMaxGrains> grains_{};
    std::array<uint16_t, kMaxGrains> free_indices_{};
    std::array<uint16_t, kMaxGrains> active_indices_{};
    std::size_t free_count_ = 0;
    std::size_t active_count_ = 0;
    FixedRandom random_{};
    uint32_t write_head_ = 0;
    uint32_t valid_history_frames_ = 0;
    uint32_t grain_counter_ = 0;
    uint32_t delta_samples_ = 0;
    uint64_t pool_exhaustion_count_ = 0;
    Vec3 last_spawned_direction_{1.0F, 0.0F, 0.0F};
    bool prepared_ = false;
    bool frozen_ = false;
    bool first_apply_ = true;
};

} // namespace iem
