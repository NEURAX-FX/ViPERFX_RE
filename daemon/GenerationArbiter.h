#pragma once

#include <cstdint>

namespace viper::daemon {

enum class ApplyDecisionKind {
    ACCEPT,
    IDEMPOTENT,
    STALE_GENERATION,
};

struct ApplyDecision {
    ApplyDecisionKind kind = ApplyDecisionKind::STALE_GENERATION;
    uint64_t resulting_daemon_generation = 0;
    uint64_t accepted_app_generation = 0;
};

ApplyDecision DecideAppApply(
    uint64_t base_daemon_generation,
    uint64_t app_generation,
    uint64_t stored_app_generation,
    uint64_t stored_daemon_generation
) noexcept;

} // namespace viper::daemon
