#pragma once

#include <cstdint>

namespace iem::resources {

struct Ku100Resource { uint32_t order; uint32_t input_channels; uint32_t frames; const float *ir; const char *source_sha256; const char *output_sha256; };
struct HeadphoneEqResource { int32_t id; const char *display_name; uint32_t channels; uint32_t frames; const float *impulse; const char *source_sha256; const char *output_sha256; };
struct DialogNetResource { uint32_t connection_count; const float *weights; const char *source_sha256; };

constexpr uint32_t kKu100ResourceCount = 3;
constexpr uint32_t kHeadphoneEqResourceCount = 23;
extern const char kUpstreamRepository[];
extern const char kUpstreamCommit[];
extern const char kKu100Attribution[];
extern const char kRenderingAttribution[];
const Ku100Resource *FindKu100(uint32_t order) noexcept;
const HeadphoneEqResource *FindHeadphoneEq(int32_t id) noexcept;
const DialogNetResource &DialogNet() noexcept;

} // namespace iem::resources
