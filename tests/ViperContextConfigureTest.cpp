// Configure() must accept every buffer configuration AudioFlinger actually hands
// the effect, not just the 48000/16-bit one the other tests use.
//
// Device evidence that motivated this: on an A2DP output thread the effect was
// configured at 96000 Hz / 4096 frames / PCM_FLOAT, and the driver then answered
// PARAM_GET_CONFIGURE=0 with PARAM_GET_SAMPLING_RATE=0. That pair is only
// reachable when Configure() prepared no graph, so the effect was attached and
// enabled while processing nothing.
#include "AudioFormat.h"
#include "ViPERParams.h"
#include "ViperContext.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr int32_t kParamGetConfigure = 2;
constexpr int32_t kParamGetSamplingRate = 4;

bool Configure(
    ViperContext &context,
    uint32_t sample_rate,
    size_t frame_count,
    uint32_t format
) {
    effect_config_t config{};
    config.input_cfg.buffer.frame_count = frame_count;
    config.input_cfg.sampling_rate = sample_rate;
    config.input_cfg.channels = AUDIO_CHANNEL_OUT_STEREO;
    config.input_cfg.format = format;
    config.input_cfg.access_mode = EFFECT_BUFFER_ACCESS_WRITE;
    config.input_cfg.mask = EFFECT_CONFIG_ALL & ~EFFECT_CONFIG_PROVIDER;
    config.output_cfg = config.input_cfg;

    int32_t reply = -1;
    uint32_t reply_size = sizeof(reply);
    return context.HandleCommand(
               EFFECT_CMD_SET_CONFIG, sizeof(config), &config, &reply_size, &reply)
        == 0;
}

// Mirrors the App/owner read path: a 4-byte param id in, a 4-byte value out.
int32_t QueryInt(ViperContext &context, int32_t param) {
    std::vector<uint8_t> request(sizeof(effect_param_t) + sizeof(int32_t), 0U);
    auto *request_param = reinterpret_cast<effect_param_t *>(request.data());
    request_param->psize = sizeof(int32_t);
    request_param->vsize = sizeof(int32_t);
    *reinterpret_cast<int32_t *>(request_param->data) = param;

    std::vector<uint8_t> reply(
        sizeof(effect_param_t) + 2U * sizeof(int32_t) + sizeof(int32_t), 0U);
    uint32_t reply_size = static_cast<uint32_t>(reply.size());
    const int32_t status = context.HandleCommand(
        EFFECT_CMD_GET_PARAM,
        static_cast<uint32_t>(request.size()),
        request.data(),
        &reply_size,
        reply.data()
    );
    assert(status == 0);

    const auto *reply_param = reinterpret_cast<const effect_param_t *>(reply.data());
    assert(reply_param->status == 0);
    const uint32_t offset =
        ((reply_param->psize + sizeof(int32_t) - 1) / sizeof(int32_t)) * sizeof(int32_t);
    return *reinterpret_cast<const int32_t *>(reply_param->data + offset);
}

// The pair the device reported. Checked together because either one alone is
// ambiguous: configure=0 could be a rejected config, and rate=0 could be a graph
// that has not been asked to prepare yet.
void AssertConfigured(ViperContext &context, uint32_t expected_rate, const char *label) {
    const int32_t configured = QueryInt(context, kParamGetConfigure);
    const int32_t rate = QueryInt(context, kParamGetSamplingRate);
    if (configured != 1 || rate != static_cast<int32_t>(expected_rate)) {
        std::fprintf(
            stderr,
            "%s: configure=%d rate=%d (expected configure=1 rate=%u)\n",
            label,
            configured,
            rate,
            expected_rate
        );
    }
    assert(configured == 1);
    assert(rate == static_cast<int32_t>(expected_rate));
}

// Control: the configuration every other test uses.
void TestSpeaker48000Sixteenbit() {
    ViperContext context;
    assert(Configure(context, 48000, viper::audio::kMaxBlockFrames, AUDIO_FORMAT_PCM_16_BIT));
    AssertConfigured(context, 48000, "48000/8192/pcm16");
}

// The configuration observed on a live A2DP output thread.
void TestA2dp96000Float() {
    ViperContext context;
    assert(Configure(context, 96000, 4096, AUDIO_FORMAT_PCM_FLOAT));
    AssertConfigured(context, 96000, "96000/4096/float");
}

// A rate change on an existing context is the common case: AudioFlinger
// reconfigures the same effect instance when the output thread switches rate.
void TestRateChangeOnConfiguredContext() {
    ViperContext context;
    assert(Configure(context, 48000, 4096, AUDIO_FORMAT_PCM_FLOAT));
    AssertConfigured(context, 48000, "48000/4096/float");

    assert(Configure(context, 96000, 4096, AUDIO_FORMAT_PCM_FLOAT));
    AssertConfigured(context, 48000, "48000 -> 96000 reconfigure");
}

// A route/rate transition can deliver SET_CONFIG more than once before the next
// audio callback consumes the pending graph. The newest request must replace the
// stale pending graph; it must not put the effect into configure=0.
void TestRepeatedReconfigureBeforeAudioDoesNotDisable() {
    ViperContext context;
    assert(Configure(context, 48000, 4096, AUDIO_FORMAT_PCM_FLOAT));
    AssertConfigured(context, 48000, "initial 48000/4096/float");

    assert(Configure(context, 96000, 4096, AUDIO_FORMAT_PCM_FLOAT));
    // The new graph is pending until Process() consumes it, so the active graph
    // still reports the old rate, but configuration must remain valid.
    AssertConfigured(context, 48000, "first pending 96000 reconfigure");

    // This is the device failure sequence: a second SET_CONFIG arrives before
    // audio has consumed the first pending graph.
    assert(Configure(context, 96000, 4096, AUDIO_FORMAT_PCM_FLOAT));
    AssertConfigured(context, 48000, "repeated pending 96000 reconfigure");
}

} // namespace

int main() {
    TestSpeaker48000Sixteenbit();
    TestA2dp96000Float();
    TestRateChangeOnConfiguredContext();
    TestRepeatedReconfigureBeforeAudioDoesNotDisable();
    std::puts("viper context configure tests passed");
    return 0;
}
