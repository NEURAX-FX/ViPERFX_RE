#include "iem/IemEngine.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

namespace {

std::atomic<uint64_t> g_new_calls{0};
std::atomic<bool> g_count_new{false};

bool Check(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", message);
    return false;
}

iem::IemEngineConfig Config(uint32_t rate, std::size_t max_block = 8192) {
    return {rate, max_block, 96000, 256, 16};
}

bool TestInvalidConfigurations() {
    iem::IemEngine engine;
    iem::IemParams params{};
    return Check(!engine.Prepare(Config(7999), params), "reject low host rate")
        && Check(!engine.Prepare(Config(384001), params), "reject high host rate")
        && Check(!engine.Prepare(Config(48000, 8193), params), "reject oversized block")
        && Check(!engine.Prepare({48000, 8192, 48000, 256, 16}, params), "reject non-96k rate")
        && Check(!engine.Prepare({48000, 8192, 96000, 128, 16}, params), "reject non-256 quantum");
}

bool TestDisabledIsBitIdenticalAtAllRates() {
    for (const uint32_t rate : {8000U, 44100U, 48000U, 96000U, 192000U, 384000U}) {
        iem::IemEngine engine;
        iem::IemParams params{};
        if (!Check(engine.Prepare(Config(rate), params), "prepare disabled engine")) return false;
        std::vector<float> audio(8192 * 2);
        for (std::size_t index = 0; index < audio.size(); ++index) {
            audio[index] = static_cast<float>(static_cast<int>(index % 257) - 128) / 128.0F;
        }
        const auto original = audio;
        if (!Check(engine.Process(audio.data(), 8192), "process disabled engine")) return false;
        if (!Check(audio == original, "disabled engine is bit-identical")) return false;
    }
    return true;
}

bool TestEnabledSpatialSineAtAllRates() {
    for (const uint32_t rate : {8000U, 44100U, 48000U, 96000U, 192000U, 384000U}) {
        iem::IemParams params{};
        params.enable = true;
        iem::IemEngine engine;
        if (!Check(engine.Prepare(Config(rate, 1024), params), "prepare enabled engine")) return false;

        const std::size_t total_frames = rate / 2;
        std::vector<float> rendered;
        rendered.reserve(total_frames);
        std::size_t position = 0;
        while (position < total_frames) {
            const std::size_t frames = std::min<std::size_t>(1024, total_frames - position);
            std::vector<float> block(frames * 2);
            for (std::size_t frame = 0; frame < frames; ++frame) {
                const float sample = static_cast<float>(std::sin(
                    2.0 * 3.14159265358979323846 * 997.0 * (position + frame) / rate
                ));
                block[frame * 2] = sample;
                block[frame * 2 + 1] = -sample;
            }
            if (!Check(engine.Process(block.data(), frames), "process enabled sine")) return false;
            for (std::size_t frame = 0; frame < frames; ++frame) {
                if (!std::isfinite(block[frame * 2]) || !std::isfinite(block[frame * 2 + 1])) {
                    return Check(false, "enabled output stays finite");
                }
                rendered.push_back(block[frame * 2]);
            }
            position += frames;
        }

        const std::size_t skip = std::min<std::size_t>(
            rendered.size(), engine.LatencyFrames() + rate / 20
        );
        double energy = 0.0;
        for (std::size_t frame = skip; frame < rendered.size(); ++frame) {
            energy += static_cast<double>(rendered[frame]) * rendered[frame];
        }
        const std::size_t count = rendered.size() - skip;
        if (!Check(count > 0, "retain settled sine samples")) return false;
        const double rms = std::sqrt(energy / count);
        if (!Check(rms > 0.01 && rms <= 1.0, "spatial sine stays non-silent and bounded")) {
            std::fprintf(
                stderr,
                "rate=%u rms=%f latency=%u\n",
                rate,
                rms,
                engine.LatencyFrames()
            );
            return false;
        }
    }
    return true;
}

bool TestWetZeroProducesDelayedDry() {
    iem::IemParams params{};
    params.enable = true;
    params.wet = 0.0F;
    params.limiter.enabled = false;
    iem::IemEngine engine;
    if (!engine.Prepare(Config(48000, 256), params)) return false;
    const std::size_t total = engine.LatencyFrames() + 512;
    std::vector<float> output;
    output.reserve(total);
    std::size_t position = 0;
    while (position < total) {
        const std::size_t frames = std::min<std::size_t>(256, total - position);
        std::vector<float> block(frames * 2, 0.0F);
        if (position == 0) {
            block[0] = 1.0F;
            block[1] = 1.0F;
        }
        if (!engine.Process(block.data(), frames)) return false;
        for (std::size_t frame = 0; frame < frames; ++frame) output.push_back(block[frame * 2]);
        position += frames;
    }
    std::size_t peak_frame = 0;
    float peak = 0.0F;
    for (std::size_t frame = 0; frame < output.size(); ++frame) {
        if (std::fabs(output[frame]) > peak) {
            peak = std::fabs(output[frame]);
            peak_frame = frame;
        }
    }
    const auto offset = static_cast<long long>(peak_frame)
        - static_cast<long long>(engine.LatencyFrames());
    return Check(std::llabs(offset) <= 1, "wet zero peak matches reported dry latency")
        && Check(peak > 0.5F && peak <= 1.0F, "wet zero impulse remains bounded");
}

bool TestOutputGainAndArbitraryCallbacks() {
    iem::IemParams params{};
    params.enable = true;
    params.wet = 0.0F;
    params.output_gain_db = 6.0F;
    iem::IemEngine engine;
    if (!engine.Prepare(Config(96000, 1024), params)) return false;
    const std::array<std::size_t, 6> chunks{1, 7, 63, 256, 1023, 511};
    const std::size_t total = engine.LatencyFrames() + 8192;
    std::size_t position = 0;
    std::size_t chunk_index = 0;
    std::vector<float> output;
    output.reserve(total);
    while (position < total) {
        const std::size_t frames = std::min(chunks[chunk_index % chunks.size()], total - position);
        std::vector<float> block(frames * 2, 0.25F);
        if (!Check(engine.Process(block.data(), frames), "process arbitrary callback")) return false;
        for (std::size_t frame = 0; frame < frames; ++frame) output.push_back(block[frame * 2]);
        position += frames;
        ++chunk_index;
    }
    const float expected = 0.25F * std::pow(10.0F, 6.0F / 20.0F);
    for (std::size_t frame = engine.LatencyFrames() + 512; frame < output.size(); ++frame) {
        if (std::fabs(output[frame] - expected) > 1.0e-3F) {
            return Check(false, "apply output gain after latency");
        }
    }
    return true;
}

bool TestProcessDoesNotAllocate() {
    iem::IemParams params{};
    params.enable = true;
    iem::IemEngine engine;
    if (!engine.Prepare(Config(48000, 1024), params)) return false;
    std::vector<float> block(1024 * 2, 0.1F);
    const uint64_t before = g_new_calls.load(std::memory_order_relaxed);
    g_count_new.store(true, std::memory_order_release);
    const bool processed = engine.Process(block.data(), 1024);
    g_count_new.store(false, std::memory_order_release);
    const uint64_t after = g_new_calls.load(std::memory_order_relaxed);
    return Check(processed, "process allocation fixture")
        && Check(before == after, "IemEngine::Process performs no operator new allocation");
}

} // namespace

void *operator new(std::size_t size) {
    if (g_count_new.load(std::memory_order_acquire)) {
        g_new_calls.fetch_add(1, std::memory_order_relaxed);
    }
    if (void *memory = std::malloc(size)) return memory;
    std::abort();
}

void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }

int main() {
    if (!TestInvalidConfigurations()) return 1;
    if (!TestDisabledIsBitIdenticalAtAllRates()) return 1;
    if (!TestEnabledSpatialSineAtAllRates()) return 1;
    if (!TestWetZeroProducesDelayedDry()) return 1;
    if (!TestOutputGainAndArbitraryCallbacks()) return 1;
    if (!TestProcessDoesNotAllocate()) return 1;
    std::puts("IEM engine tests passed");
    return 0;
}
