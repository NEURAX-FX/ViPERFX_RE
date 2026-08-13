#include "IemResources.h"

#include "IemGraph.h"
#include "iem/IemParams.h"

#include <limits>

namespace viper::audio {

bool IemResources::CaptureRaw(int param, int val1) noexcept {
    if (param != iem::kParamIemResourceReset || val1 != 1
        || generation_ == std::numeric_limits<uint64_t>::max()) {
        return false;
    }
    ++generation_;
    return true;
}

bool IemResources::ApplyTo(IemGraph &graph) const noexcept {
    if (!graph.IsPrepared()) return false;
    graph.SetResourceGeneration(generation_);
    return true;
}

} // namespace viper::audio
