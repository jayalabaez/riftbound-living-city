#include "TestFramework.h"
#include "livingcity/world/Landing.h"
#include <limits>
using namespace lc;
LC_TEST(landing_boundaries_and_stationary_level_ground) {
    LandingObservation o;
    LC_CHECK(EvaluateLanding(o) == LandingDecision::Safe);
    o.altitudeCm=1000;o.speedCmPerSecond=500;o.minimumUpDotMilli=978;o.supportHeightsCm[8]=90;
    LC_CHECK(EvaluateLanding(o) == LandingDecision::Safe);
    ++o.altitudeCm;LC_CHECK(EvaluateLanding(o) == LandingDecision::TooHigh);
    o.altitudeCm=999;++o.speedCmPerSecond;LC_CHECK(EvaluateLanding(o) == LandingDecision::TooFast);
}
LC_TEST(landing_rejects_cliff_slope_and_blocked_footprint) {
    LandingObservation o;o.supportHeightsCm[3]=91;
    LC_CHECK(EvaluateLanding(o) == LandingDecision::Uneven);
    o.supportHeightsCm[3]=0;o.minimumUpDotMilli=977;
    LC_CHECK(EvaluateLanding(o) == LandingDecision::Steep);
    o.minimumUpDotMilli=1000;o.footprintClear=false;
    LC_CHECK(EvaluateLanding(o) == LandingDecision::Obstructed);
}
LC_TEST(landing_quantized_extremes_and_invalid_observations_fail_closed) {
    LandingObservation o;o.supportHeightsCm[0]=std::numeric_limits<i32>::min();o.supportHeightsCm[1]=std::numeric_limits<i32>::max();
    LC_CHECK(EvaluateLanding(o) == LandingDecision::Uneven);
    o={};o.finite=false;LC_CHECK(EvaluateLanding(o) == LandingDecision::Invalid);
    o={};o.altitudeCm=-1;LC_CHECK(EvaluateLanding(o) == LandingDecision::Invalid);
    o={};o.speedCmPerSecond=-1;LC_CHECK(EvaluateLanding(o) == LandingDecision::Invalid);
    o={};LandingLimits limits;limits.minimumUpDotMilli=1001;
    LC_CHECK(EvaluateLanding(o,limits) == LandingDecision::Invalid);
}
LC_TEST(landing_support_order_does_not_change_result) {
    LandingObservation o;for (i32 i=0;i<9;++i)o.supportHeightsCm[i]=i*13;
    const auto first=EvaluateLanding(o);
    for (i32 i=0;i<4;++i){const auto h=o.supportHeightsCm[i];o.supportHeightsCm[i]=o.supportHeightsCm[8-i];o.supportHeightsCm[8-i]=h;}
    LC_CHECK(EvaluateLanding(o) == first);
}
