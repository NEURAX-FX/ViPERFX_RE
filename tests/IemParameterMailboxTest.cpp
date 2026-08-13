#include "IemParameterMailbox.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>

namespace {

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

bool TestEmptyMailbox() {
    viper::audio::IemParameterMailbox mailbox;
    uint64_t generation = 0;
    iem::IemParams params{};
    return Check(!mailbox.ConsumeLatest(generation, params), "empty mailbox has no update")
        && Check(generation == 0, "empty mailbox keeps generation");
}

bool TestLatestPublicationWins() {
    viper::audio::IemParameterMailbox mailbox;
    iem::IemParams params{};
    params.wet = 0.25F;
    const uint64_t first = mailbox.Publish(params);
    params.wet = 0.75F;
    params.order = 3;
    const uint64_t second = mailbox.Publish(params);

    uint64_t generation = 0;
    iem::IemParams consumed{};
    return Check(first == 1 && second == 2, "generations increase")
        && Check(mailbox.ConsumeLatest(generation, consumed), "consume latest update")
        && Check(generation == second, "consume latest generation")
        && Check(consumed.wet == 0.75F && consumed.order == 3, "consume coherent latest params")
        && Check(!mailbox.ConsumeLatest(generation, consumed), "consume each generation once");
}

bool TestConcurrentSnapshotsStayCoherent() {
    constexpr int kPublicationCount = 10000;
    viper::audio::IemParameterMailbox mailbox;
    std::atomic<bool> done{false};
    std::atomic<int> errors{0};

    std::thread producer([&] {
        for (int marker = 1; marker <= kPublicationCount; ++marker) {
            iem::IemParams params{};
            params.wet = static_cast<float>(marker % 101) / 100.0F;
            params.output_gain_db = static_cast<float>(marker);
            params.order = static_cast<uint32_t>(marker % 3 + 1);
            mailbox.Publish(params);
        }
        done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        uint64_t generation = 0;
        iem::IemParams params{};
        while (!done.load(std::memory_order_acquire)
               || generation < kPublicationCount) {
            if (!mailbox.ConsumeLatest(generation, params)) {
                std::this_thread::yield();
                continue;
            }
            const int marker = static_cast<int>(params.output_gain_db);
            const float expected_wet = static_cast<float>(marker % 101) / 100.0F;
            const uint32_t expected_order = static_cast<uint32_t>(marker % 3 + 1);
            if (std::fabs(params.wet - expected_wet) > 1.0e-7F
                || params.order != expected_order) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    producer.join();
    consumer.join();
    return Check(errors.load(std::memory_order_relaxed) == 0, "concurrent snapshots stay coherent");
}

} // namespace

int main() {
    if (!TestEmptyMailbox()) return 1;
    if (!TestLatestPublicationWins()) return 1;
    if (!TestConcurrentSnapshotsStayCoherent()) return 1;
    std::puts("IEM parameter mailbox tests passed");
    return 0;
}
