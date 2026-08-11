#pragma once

#include <cstddef>
#include <cmath>

namespace iem {

class LinearSmoother {
public:
    void Reset(float value = 0.0F) noexcept {
        current_ = std::isfinite(value) ? value : 0.0F;
        target_ = current_;
        increment_ = 0.0F;
        remaining_ = 0;
    }

    void SetTarget(float target, std::size_t frames) noexcept {
        if (!std::isfinite(target)) target = current_;
        target_ = target;
        remaining_ = frames;
        if (remaining_ == 0) {
            current_ = target_;
            increment_ = 0.0F;
            return;
        }
        increment_ = (target_ - current_) / static_cast<float>(remaining_);
    }

    float Next() noexcept {
        if (remaining_ == 0) return current_;
        current_ += increment_;
        --remaining_;
        if (remaining_ == 0) current_ = target_;
        return current_;
    }

    float Current() const noexcept { return current_; }
    float Target() const noexcept { return target_; }
    bool IsSmoothing() const noexcept { return remaining_ != 0; }

private:
    float current_ = 0.0F;
    float target_ = 0.0F;
    float increment_ = 0.0F;
    std::size_t remaining_ = 0;
};

} // namespace iem
