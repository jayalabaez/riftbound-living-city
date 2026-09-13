// LIVING CITY — citizens.
//
// R1: zero Unreal types. R3: deterministic. R4: the player is an NPC - there is no player
// special case anywhere in this file, and there never will be.
//
// This is the Phase 2 skeleton. Every citizen has a home, a job, and a daily routine the
// player can follow: they leave home in the morning, walk to work along the street grid,
// work a shift, and walk home. Needs, utility scoring and the economy come next; the
// structure here is the struct-of-arrays those systems will extend.
//
// STRUCT OF ARRAYS, not an array of structs. Every field is its own contiguous vector so a
// system that only touches positions never pulls job records through cache. This is the
// layout that has to survive to 50,000 citizens, so it starts that way at 1,500.
#pragma once

#include "livingcity/core/Core.h"
#include "livingcity/core/Clock.h"
#include "livingcity/core/Fixed.h"
#include "livingcity/core/Rng.h"
#include "livingcity/world/City.h"

#include <vector>

namespace lc {

// Where a citizen is in their day. Drives movement and, later, which needs decay.
enum class Activity : u8 {
    AtHome     = 0,
    TravelWork = 1,
    AtWork     = 2,
    TravelHome = 3,
    TravelShop = 4,   // an errand: walking to a shop the economy chose
    AtShop     = 5,   // at the counter; heads home when done
    Count      = 6
};

LC_API const char* ActivityName(Activity a);

class LC_API Citizens {
public:
    // Assigns every citizen a home and a workplace drawn from the city.
    //
    // I4: if a citizen cannot be given both, that is a generator failure and it is LC_FATAL,
    // not a silent skip. A citizen without a home is exactly the bug this rule exists to
    // catch, and it must never reach the player as "that one just stands there".
    void Generate(u64 worldSeed, const City& city, u32 count);

    // One 20 Hz step. Movement is integer-exact along the road grid; the schedule is read
    // from the clock, never accumulated, so it cannot drift (R3).
    void Tick(const SimClock& clock, const City& city);

    u32 Count() const { return static_cast<u32>(homeIndex_.size()); }

    // ---- per-citizen state (SoA) --------------------------------------------------------
    i32      PosX(u32 i) const;        // metres from city centre
    i32      PosY(u32 i) const;
    Activity State(u32 i) const;
    u32      HomeBuilding(u32 i) const;
    u32      WorkBuilding(u32 i) const;
    i32      ShiftStartHour(u32 i) const;
    i32      ShiftEndHour(u32 i) const;

    // ---- errands ------------------------------------------------------------------------
    // The economy decides WHETHER and WHERE a citizen shops; Citizens only executes the walk.
    // That split is what keeps the layer graph acyclic: economy depends on agents, never the
    // reverse, so the decision lives above and the movement lives here.
    //
    // A pending errand fires at the citizen's next departure - at shift end, instead of
    // walking straight home, or from home outside working hours. Going to work always takes
    // priority over an errand. The trip always ends at home.
    static constexpr u32 kNoErrand       = 0xFFFFFFFFu;
    static constexpr u16 kShopDwellTicks = 600u;   // 30 sim-seconds at the counter

    void SetPendingErrand(u32 i, u32 shopBuilding);   // kNoErrand cancels
    u32  PendingErrand(u32 i) const;
    bool HasPendingErrand(u32 i) const { return PendingErrand(i) != kNoErrand; }

    // True on exactly the one tick a citizen arrives at a shop - the tick the economy rings
    // up the sale. False on every other tick, including the rest of the dwell.
    bool JustArrivedAtShop(u32 i) const;

    // ---- aggregates, for the HUD --------------------------------------------------------
    u32 CountIn(Activity a) const;

    void HashInto(Hasher& h) const;
    void Clear();

private:
    // Target the citizen is walking toward, and the leg of the L-shaped road path they are
    // on. Kept parallel to the public arrays.
    std::vector<u32> homeIndex_;      // index into City buildings
    std::vector<u32> workIndex_;
    std::vector<i32> posX_;
    std::vector<i32> posY_;
    std::vector<i32> targetX_;
    std::vector<i32> targetY_;
    std::vector<u8>  state_;          // Activity
    std::vector<u8>  leg_;            // 0 = travelling in X, 1 = travelling in Y, 2 = arrived
    std::vector<u8>  speed_;          // metres per 100 ticks; varies per citizen
    std::vector<u8>  shiftStart_;     // hour of day
    std::vector<u8>  shiftEnd_;
    std::vector<u32> errand_;         // kNoErrand, or the Shop building to visit next
    std::vector<u16> dwell_;          // ticks remaining at the shop counter

    u32 counts_[static_cast<u32>(Activity::Count)] = {};

    void BeginTravel(u32 i, const City& city, u32 destBuilding);
    void StepTravel(u32 i);
};

} // namespace lc
