#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace viper::owner {

constexpr uint16_t kOwnerProtocolVersion = 1;
constexpr const char *kOwnerSocketName = "viper4android.owner.v1";
constexpr uint32_t kOwnerMaxPayloadBytes = 4096;

enum class OwnerMessage : uint16_t {
    OWNER_HELLO = 300,
    OWNER_HELLO_ACK = 301,
    OWN_SESSION = 302,
    OWNED = 303,
    OWN_FAILED = 304,
    RELEASE_SESSION = 305,
    RELEASED = 306,
    SESSION_DELTA = 307,
};

enum class EffectTypeSelector : uint16_t {
    HIDL = 1,
    AIDL = 2,
};

constexpr std::size_t kOwnerHelloWireSize = 20;
constexpr std::size_t kOwnerHelloAckWireSize = 16;
constexpr std::size_t kOwnSessionWireSize = 16;
constexpr std::size_t kOwnedWireSize = 16;
constexpr std::size_t kOwnerFailedWireSize = 16;
constexpr std::size_t kReleaseSessionWireSize = 12;
constexpr std::size_t kReleasedWireSize = 12;
constexpr std::size_t kSessionDeltaWireSize = 16;
struct OwnerHello {
    uint64_t owner_pid = 0;
    uint64_t boot_id = 0;
};

struct OwnerHelloAck {
    bool accepted = false;
    uint64_t daemon_generation = 0;
};

struct OwnSession {
    uint32_t audio_session_id = 0;
    EffectTypeSelector selector = EffectTypeSelector::HIDL;
};

struct Owned {
    uint32_t audio_session_id = 0;
    uint32_t effect_id = 0;
    bool has_control = false;
};

struct OwnerFailed {
    uint32_t audio_session_id = 0;
    uint32_t reason_code = 0;
};

struct ReleaseSession {
    uint32_t audio_session_id = 0;
};

struct Released {
    uint32_t audio_session_id = 0;
};

struct SessionDelta {
    uint32_t audio_session_id = 0;
    uint32_t client_uid = 0;
    bool appeared = false;
};

bool IsOwnerMessage(uint16_t value) noexcept;
bool IsAllowedEffectSelector(uint16_t value) noexcept;

bool EncodeOwnerHello(const OwnerHello &, std::vector<uint8_t> *, std::string *error);
bool DecodeOwnerHello(std::span<const uint8_t>, OwnerHello *, std::string *error);

bool EncodeOwnerHelloAck(const OwnerHelloAck &, std::vector<uint8_t> *, std::string *error);
bool DecodeOwnerHelloAck(std::span<const uint8_t>, OwnerHelloAck *, std::string *error);

bool EncodeOwnSession(const OwnSession &, std::vector<uint8_t> *, std::string *error);
bool DecodeOwnSession(std::span<const uint8_t>, OwnSession *, std::string *error);

bool EncodeOwned(const Owned &, std::vector<uint8_t> *, std::string *error);
bool DecodeOwned(std::span<const uint8_t>, Owned *, std::string *error);

bool EncodeOwnerFailed(const OwnerFailed &, std::vector<uint8_t> *, std::string *error);
bool DecodeOwnerFailed(std::span<const uint8_t>, OwnerFailed *, std::string *error);

bool EncodeReleaseSession(const ReleaseSession &, std::vector<uint8_t> *, std::string *error);
bool DecodeReleaseSession(std::span<const uint8_t>, ReleaseSession *, std::string *error);

bool EncodeReleased(const Released &, std::vector<uint8_t> *, std::string *error);
bool DecodeReleased(std::span<const uint8_t>, Released *, std::string *error);

bool EncodeSessionDelta(const SessionDelta &, std::vector<uint8_t> *, std::string *error);
bool DecodeSessionDelta(std::span<const uint8_t>, SessionDelta *, std::string *error);

} // namespace viper::owner
