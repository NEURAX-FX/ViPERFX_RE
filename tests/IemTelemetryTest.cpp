#include "TelemetryProtocol.h"
#include "iem/IemTelemetry.h"

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

bool TestSnapshotAndWireMapping() {
    iem::IemTelemetryPublisher publisher;
    publisher.Configure(48000, 96000);
    publisher.RecordSpatialState(
        3, 0, 3, 17, 9, 1, 1536,
        iem::IemPreparationResult::SUCCESS, 4, 2.5F);
    publisher.RecordQueueOverflow(true);
    publisher.RecordQueueOverflow(false);
    publisher.RecordBlock(256, 1200000, 5333333, 1024, true);
    publisher.RecordBlock(256, 6000000, 5333333, 1024, true);
    publisher.RecordFailure(iem::IemBypassReason::OUTPUT_UNDERFLOW);

    iem::IemTelemetrySnapshot snapshot{};
    if (!Check(publisher.Read(snapshot), "read configured snapshot")) return false;
    if (!Check(snapshot.processed_frames == 512, "accumulate processed frames")) return false;
    if (!Check(snapshot.latest_process_ns == 6000000, "record latest process time")) return false;
    if (!Check(snapshot.average_process_ns == 3600000, "compute average process time")) return false;
    if (!Check(snapshot.max_process_ns == 6000000, "record maximum process time")) return false;
    if (!Check(snapshot.deadline_misses == 1, "record deadline miss")) return false;
    if (!Check(snapshot.output_underflows == 1, "record output underflow")) return false;
    if (!Check(snapshot.input_overflows == 1 && snapshot.output_overflows == 1,
            "record queue overflows")) return false;
    if (!Check(snapshot.encoder_mode == 3 && snapshot.render_mode == 0
            && snapshot.ambisonics_order == 3
            && snapshot.active_grains == 17 && snapshot.grain_pool_exhaustions == 9,
            "record spatial state")) return false;
    if (!Check(snapshot.halo_prepared == 1
            && snapshot.halo_stft_latency_frames == 1536
            && snapshot.dialog_net_result == iem::IemPreparationResult::SUCCESS,
            "record Halo preparation state")) return false;
    if (!Check(snapshot.fault_code == 4
            && std::fabs(snapshot.limiter_gain_reduction_db - 2.5F) < 1.0e-6F,
            "record fault and limiter state")) return false;
    if (!Check(snapshot.bypass_reason == iem::IemBypassReason::OUTPUT_UNDERFLOW,
               "record bypass reason")) return false;
    if (!Check(snapshot.host_sample_rate == 48000 && snapshot.internal_sample_rate == 96000,
               "record sample rates")) return false;
    if (!Check(snapshot.latency_frames == 1024 && snapshot.enabled && snapshot.prepared,
               "record state fields")) return false;

    const viper::IemTelemetryWire wire = viper::MakeIemTelemetryWire(snapshot);
    static_assert(sizeof(viper::TelemetryWire) == 320);
    static_assert(sizeof(viper::IemTelemetryWire) == 200);
    return Check(wire.version == viper::kIemTelemetryVersion, "map wire version")
        && Check(wire.wire_size == sizeof(wire), "map wire size")
        && Check(wire.processed_frames == snapshot.processed_frames, "map processed frames")
        && Check(wire.bypass_reason == static_cast<uint32_t>(snapshot.bypass_reason),
                  "map bypass reason")
        && Check(wire.encoder_mode == 3 && wire.render_mode == 0
            && wire.ambisonics_order == 3,
            "map spatial state")
        && Check(wire.halo_prepared == 1 && wire.halo_stft_latency_frames == 1536
            && wire.dialog_net_result
                == static_cast<uint32_t>(iem::IemPreparationResult::SUCCESS),
            "map Halo preparation state")
        && Check(wire.input_overflows == 1 && wire.output_overflows == 1,
            "map queue overflow state")
        && Check(std::fabs(wire.latency_ms - 21.333333F) < 0.001F,
            "map latency milliseconds")
        && Check(wire.enabled == 1 && wire.prepared == 1, "map state flags");
}

bool TestConcurrentReadsStayCoherent() {
    constexpr uint64_t kIterations = 10000;
    iem::IemTelemetryPublisher publisher;
    publisher.Configure(96000, 96000);
    std::atomic<bool> done{false};
    std::atomic<int> errors{0};

    std::thread writer([&] {
        for (uint64_t marker = 1; marker <= kIterations; ++marker) {
            publisher.RecordBlock(
                1,
                marker,
                kIterations + 1,
                static_cast<uint32_t>(marker % 4096),
                (marker & 1U) != 0
            );
        }
        done.store(true, std::memory_order_release);
    });

    std::thread reader([&] {
        while (!done.load(std::memory_order_acquire)) {
            iem::IemTelemetrySnapshot snapshot{};
            if (!publisher.Read(snapshot) || snapshot.latest_process_ns == 0) continue;
            if (snapshot.latency_frames != snapshot.latest_process_ns % 4096
                || snapshot.enabled != ((snapshot.latest_process_ns & 1U) != 0)) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    writer.join();
    reader.join();
    iem::IemTelemetrySnapshot final_snapshot{};
    return Check(errors.load(std::memory_order_relaxed) == 0,
                 "concurrent reads never mix generations")
        && Check(publisher.Read(final_snapshot), "read final snapshot")
        && Check(final_snapshot.processed_frames == kIterations,
                 "publish every processed frame");
}

} // namespace

int main() {
    if (!TestSnapshotAndWireMapping()) return 1;
    if (!TestConcurrentReadsStayCoherent()) return 1;
    std::puts("IEM telemetry tests passed");
    return 0;
}
