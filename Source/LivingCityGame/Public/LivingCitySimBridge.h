// The single door between Unreal and the simulation core.
//
// Every Unreal-side file that needs sim types includes THIS, never the sim headers
// directly. That gives us exactly one place to suppress third-party-style warnings, one
// place to see the whole surface area of the boundary, and one file to inspect when asking
// "what does the engine actually know about the sim?".
//
// Rule R2: engine -> sim is INPUT ONLY (commands). Sim -> engine is STATE ONLY (snapshots).
#pragma once

#include "CoreMinimal.h"

// The sim core is plain C++20 and knows nothing about Unreal's warning settings.
THIRD_PARTY_INCLUDES_START
#include "livingcity/sim/Sim.h"
#include "livingcity/sim/Snapshot.h"
#include "livingcity/sim/Commands.h"
THIRD_PARTY_INCLUDES_END

/**
 * Converts a Q32.32 fixed-point raw value to float for rendering.
 *
 * This is the ONLY place fixed-point becomes floating point. The sim is integer-exact;
 * float appears at the last possible moment, on the presentation side of the boundary,
 * where a rounding difference cannot affect simulation state (rule R3).
 */
FORCEINLINE float LivingCityFixedRawToFloat(int64 Raw)
{
    // 2^32 as a double first, so the division is exact before narrowing.
    return static_cast<float>(static_cast<double>(Raw) / 4294967296.0);
}

/** Sim world units are metres; Unreal works in centimetres. */
FORCEINLINE FVector LivingCitySnapshotToWorld(const lc::Snapshot& Snap)
{
    return FVector(
        LivingCityFixedRawToFloat(Snap.placeholderX) * 100.0f,
        LivingCityFixedRawToFloat(Snap.placeholderY) * 100.0f,
        LivingCityFixedRawToFloat(Snap.placeholderZ) * 100.0f);
}
