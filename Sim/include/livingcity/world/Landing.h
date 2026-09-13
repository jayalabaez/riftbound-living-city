#pragma once
#include "livingcity/core/Core.h"
#include <array>

namespace lc {
// Quantized geometry observations supplied by the host. No engine or physics state
// crosses this boundary; the suitability decision is deterministic and allocation-free.
struct LandingLimits {
    i32 maxAltitudeCm = 1000;
    i32 maxSpeedCmPerSecond = 500;
    i32 maxReliefCm = 90;
    i32 minimumUpDotMilli = 978;
};
struct LandingObservation {
    i32 altitudeCm = 0;
    i32 speedCmPerSecond = 0;
    std::array<i32, 9> supportHeightsCm{};
    i32 minimumUpDotMilli = 1000;
    bool finite = true;
    bool footprintClear = true;
};
enum class LandingDecision : u8 { Safe, Invalid, TooHigh, TooFast, Steep, Uneven, Obstructed };
inline LandingDecision EvaluateLanding(const LandingObservation& o, const LandingLimits& limits = {}) {
    if (!o.finite || o.altitudeCm < 0 || o.speedCmPerSecond < 0 ||
        o.minimumUpDotMilli < -1000 || o.minimumUpDotMilli > 1000 ||
        limits.maxAltitudeCm <= 0 || limits.maxSpeedCmPerSecond <= 0 ||
        limits.maxReliefCm < 0 || limits.minimumUpDotMilli < 0 || limits.minimumUpDotMilli > 1000)
        return LandingDecision::Invalid;
    if (o.altitudeCm > limits.maxAltitudeCm) return LandingDecision::TooHigh;
    if (o.speedCmPerSecond > limits.maxSpeedCmPerSecond) return LandingDecision::TooFast;
    if (o.minimumUpDotMilli < limits.minimumUpDotMilli) return LandingDecision::Steep;
    i32 low = o.supportHeightsCm[0], high = low;
    for (const i32 h : o.supportHeightsCm) { if (h < low) low = h; if (h > high) high = h; }
    if (static_cast<i64>(high) - low > limits.maxReliefCm) return LandingDecision::Uneven;
    if (!o.footprintClear) return LandingDecision::Obstructed;
    return LandingDecision::Safe;
}
}
