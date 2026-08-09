#include "ParameterMailbox.h"
#include <atomic>
#include <cstdio>
#include <thread>

namespace {

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

viper::ViPERParams MarkerParams(uint32_t marker) {
    viper::ViPERParams params{};
    params.bass.frequency = marker;
    params.dynamic_eq.bands[0].frequency = static_cast<float>(marker);
    params.master_limiter.output_volume = static_cast<float>(marker);
    return params;
}

bool IsConsistent(const viper::ViPERParams &params) {
    const uint32_t marker = params.bass.frequency;
    return params.dynamic_eq.bands[0].frequency == static_cast<float>(marker)
        && params.master_limiter.output_volume == static_cast<float>(marker);
}

bool TestConsumesLatestPublishedSnapshot() {
    viper::audio::ParameterMailbox mailbox;
    mailbox.Publish(MarkerParams(11));
    mailbox.Publish(MarkerParams(22));
    uint64_t generation = 0;
    viper::ViPERParams result{};
    if (!Check(mailbox.ConsumeLatest(generation, result), "consume published snapshot")) {
        return false;
    }
    return Check(result.bass.frequency == 22, "consume newest snapshot")
        && Check(generation == 2, "report newest generation")
        && Check(!mailbox.ConsumeLatest(generation, result), "do not replay same generation");
}

bool TestConcurrentReadsNeverObserveTornSnapshot() {
    viper::audio::ParameterMailbox mailbox;
    std::atomic<bool> done{false};
    std::atomic<bool> consistent{true};
    std::thread writer([&]() {
        for (uint32_t marker = 1; marker <= 20000; ++marker) {
            mailbox.Publish(MarkerParams(marker));
        }
        done.store(true, std::memory_order_release);
    });

    uint64_t generation = 0;
    viper::ViPERParams result{};
    while (!done.load(std::memory_order_acquire)) {
        if (mailbox.ConsumeLatest(generation, result) && !IsConsistent(result)) {
            consistent.store(false, std::memory_order_relaxed);
            break;
        }
    }
    writer.join();
    while (mailbox.ConsumeLatest(generation, result)) {
        if (!IsConsistent(result)) consistent.store(false, std::memory_order_relaxed);
    }
    return Check(consistent.load(), "reader never observes a torn POD snapshot")
        && Check(generation == 20000, "reader reaches final generation");
}

} // namespace

int main() {
    if (!TestConsumesLatestPublishedSnapshot()) return 1;
    if (!TestConcurrentReadsNeverObserveTornSnapshot()) return 1;
    std::puts("Parameter mailbox tests passed");
    return 0;
}
