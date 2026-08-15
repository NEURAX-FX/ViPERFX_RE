#pragma once

#include <cstdint>

namespace viper::audio {

class IemGraph;

class IemResources final {
public:
    uint64_t Generation() const noexcept { return generation_; }
    void RestoreGeneration(uint64_t generation) noexcept { generation_ = generation; }
    bool CaptureRaw(int param, int val1) noexcept;
    bool ApplyTo(IemGraph &graph) const noexcept;

private:
    uint64_t generation_ = 0;
};

} // namespace viper::audio
