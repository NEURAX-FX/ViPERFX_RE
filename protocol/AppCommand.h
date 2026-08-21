#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace viper::daemon {

// App-to-daemon protocol on the private abstract socket @viper4android.app.v1.
//
// Separate from @viper4android.driver.v1 on purpose: the driver socket only
// admits root/audioserver, because a driver event is trusted lifecycle data. The
// App is an ordinary uid and must not be able to forge those, so it gets its own
// endpoint with its own admission rule.
//
// Snapshot streaming reuses SnapshotCommandType (100-103) and the SnapshotCommand
// codecs unchanged: the App already produces exactly those payloads, and a second
// snapshot format would be one more thing to drift.
constexpr uint16_t kAppProtocolVersion = 1;
constexpr const char *kAppSocketName = "viper4android.app.v1";

// Message types live above the snapshot command range, so one dispatcher can tell
// an app control message from a snapshot command by frame header alone.
enum class AppMessageType : uint16_t {
    APP_HELLO = 200,
    APP_HELLO_ACK = 201,
    APP_ROUTE_REPORT = 202,
    APP_ROUTE_ACK = 203,
    APP_APPLY_RESULT = 204,
};

bool IsAppMessageType(uint16_t value) noexcept;

constexpr std::size_t kAppHelloWireSize = 24;
constexpr std::size_t kAppHelloAckWireSize = 96;
constexpr std::size_t kAppRouteReportHeaderSize = 32;
constexpr std::size_t kAppRouteAckWireSize = 96;
constexpr std::size_t kAppApplyResultWireSize = 48;

// Bounded so a buggy or hostile App cannot make the root daemon allocate freely.
constexpr std::size_t kMaxAppRouteFieldBytes = 256;
constexpr std::size_t kAppDeviceHashSize = 64;

// Flags in AppHelloAck::flags and AppRouteAck.
constexpr uint16_t kAppFlagRestoreEnabled = 1U << 0U;
constexpr uint16_t kAppFlagDriverConnected = 1U << 1U;
constexpr uint16_t kAppFlagRouteKnown = 1U << 2U;

struct AppHello {
    uint16_t version = kAppProtocolVersion;
    uint64_t app_generation = 0;
};

struct AppHelloAck {
    uint16_t version = kAppProtocolVersion;
    uint16_t flags = 0;
    uint64_t daemon_generation = 0;
    uint64_t route_epoch = 0;
    // Empty when the daemon has no route yet.
    std::string route_key_hash;
};

// The App is the only component that can see the real output route: the daemon
// cannot read the live mixer without hidden AudioFlinger APIs, and many devices
// (including MTK mt6989) expose no headset switch node in sysfs at all.
struct AppRouteReport {
    uint16_t version = kAppProtocolVersion;
    std::string route_type;
    std::string stable_address_or_port;
    std::string product_name;
    std::string encoding;
    uint32_t sample_rate = 0;
    uint32_t channel_mask = 0;
    uint32_t output_flags = 0;
};

struct AppRouteAck {
    bool accepted = false;
    uint64_t daemon_generation = 0;
    uint64_t route_epoch = 0;
    std::string route_key_hash;
};

struct AppApplyResult {
    bool accepted = false;
    uint32_t error_code = 0;
    uint64_t app_generation = 0;
    uint64_t daemon_generation = 0;
    uint64_t resource_generation = 0;
    uint64_t graph_generation = 0;
};

bool EncodeAppHello(const AppHello &hello, std::vector<uint8_t> *out, std::string *error);
bool DecodeAppHello(std::span<const uint8_t> bytes, AppHello *hello, std::string *error);

bool EncodeAppHelloAck(const AppHelloAck &ack, std::vector<uint8_t> *out, std::string *error);
bool DecodeAppHelloAck(std::span<const uint8_t> bytes, AppHelloAck *ack, std::string *error);

bool EncodeAppRouteReport(
    const AppRouteReport &report,
    std::vector<uint8_t> *out,
    std::string *error
);
bool DecodeAppRouteReport(
    std::span<const uint8_t> bytes,
    AppRouteReport *report,
    std::string *error
);

bool EncodeAppRouteAck(const AppRouteAck &ack, std::vector<uint8_t> *out, std::string *error);
bool DecodeAppRouteAck(std::span<const uint8_t> bytes, AppRouteAck *ack, std::string *error);

bool EncodeAppApplyResult(
    const AppApplyResult &result,
    std::vector<uint8_t> *out,
    std::string *error
);
bool DecodeAppApplyResult(
    std::span<const uint8_t> bytes,
    AppApplyResult *result,
    std::string *error
);

} // namespace viper::daemon
