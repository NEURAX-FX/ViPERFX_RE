#include "GenerationArbiter.h"

#include <cassert>
#include <limits>

namespace {

using viper::daemon::ApplyDecisionKind;
using viper::daemon::DecideAppApply;

void TestFirstAndNewerAppApply() {
    auto decision = DecideAppApply(0, 1, 0, 0);
    assert(decision.kind == ApplyDecisionKind::ACCEPT);
    assert(decision.resulting_daemon_generation == 1);
    assert(decision.accepted_app_generation == 1);

    decision = DecideAppApply(1, 3, 1, 1);
    assert(decision.kind == ApplyDecisionKind::ACCEPT);
    assert(decision.resulting_daemon_generation == 2);
    assert(decision.accepted_app_generation == 3);
}

void TestIdempotentAndStaleApply() {
    auto decision = DecideAppApply(5, 7, 7, 5);
    assert(decision.kind == ApplyDecisionKind::IDEMPOTENT);
    assert(decision.resulting_daemon_generation == 5);

    decision = DecideAppApply(4, 8, 7, 5);
    assert(decision.kind == ApplyDecisionKind::STALE_GENERATION);

    decision = DecideAppApply(5, 6, 7, 5);
    assert(decision.kind == ApplyDecisionKind::STALE_GENERATION);

    decision = DecideAppApply(0, 7, 7, 5);
    assert(decision.kind == ApplyDecisionKind::STALE_GENERATION);
}

void TestRejectsZeroAndOverflow() {
    auto decision = DecideAppApply(0, 0, 0, 0);
    assert(decision.kind == ApplyDecisionKind::STALE_GENERATION);

    decision = DecideAppApply(
        std::numeric_limits<uint64_t>::max(),
        2,
        1,
        std::numeric_limits<uint64_t>::max()
    );
    assert(decision.kind == ApplyDecisionKind::STALE_GENERATION);
}

} // namespace

int main() {
    TestFirstAndNewerAppApply();
    TestIdempotentAndStaleApply();
    TestRejectsZeroAndOverflow();
    return 0;
}
