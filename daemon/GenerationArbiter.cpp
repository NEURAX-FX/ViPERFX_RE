#include "GenerationArbiter.h"

#include <limits>

namespace viper::daemon {

ApplyDecision DecideAppApply(
    uint64_t base_daemon_generation,
    uint64_t app_generation,
    uint64_t stored_app_generation,
    uint64_t stored_daemon_generation
) noexcept {
    ApplyDecision decision{};
    decision.resulting_daemon_generation = stored_daemon_generation;
    decision.accepted_app_generation = stored_app_generation;

    if (app_generation == 0U
        || base_daemon_generation != stored_daemon_generation
        || app_generation < stored_app_generation
        || stored_daemon_generation == std::numeric_limits<uint64_t>::max()) {
        return decision;
    }
    if (app_generation == stored_app_generation) {
        decision.kind = ApplyDecisionKind::IDEMPOTENT;
        return decision;
    }

    decision.kind = ApplyDecisionKind::ACCEPT;
    decision.resulting_daemon_generation = stored_daemon_generation + 1U;
    decision.accepted_app_generation = app_generation;
    return decision;
}

} // namespace viper::daemon
