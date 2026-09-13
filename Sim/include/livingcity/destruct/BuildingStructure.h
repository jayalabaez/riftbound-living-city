#pragma once
#include "livingcity/core/Core.h"
#include <algorithm>
#include <array>
#include <span>

namespace lc {
// A bounded storey chain: four regional supports carry each floor and its upper
// neighbours. Positions are quantized by the host before becoming commands.
constexpr i32 StructureMaxFloors = 18;
constexpr i32 StructureSupportsPerFloor = 4;
constexpr i32 StructureSupportCount = StructureMaxFloors * StructureSupportsPerFloor;
constexpr i32 StructureFullIntegrity = 120000; // hundredths of legacy integrity
constexpr u32 StructureGlassMask = (1u << StructureMaxFloors) - 1u;

struct StructureLimits {
    i32 supportStrength = 18000;
    i32 requiredSupports = 3;
};
struct BuildingStructure {
    i32 integrity = StructureFullIntegrity;
    u32 brokenGlassFloors = 0;
    std::array<u16, StructureSupportCount> supportDamage{};
    i32 collapsedFromFloor = StructureMaxFloors;
};
enum class StructureCondition { Intact, Damaged, Severe, PartialCollapse, MajorCollapse };

inline i32 LegacyStandingFloors(i32 integrity, i32 floors) {
    floors = std::clamp(floors, 0, StructureMaxFloors);
    if (floors == 0 || integrity <= 0) return 0;
    if (integrity <= 30000) return std::max(1, floors / 3);
    if (integrity <= 65000) return std::max(1, floors * 2 / 3);
    return floors;
}
inline i32 StandingFloors(const BuildingStructure& state, i32 floors) {
    return std::min(LegacyStandingFloors(state.integrity, floors),
        std::clamp(state.collapsedFromFloor, 0, StructureMaxFloors));
}
inline bool ValidStructure(const BuildingStructure& state) {
    return state.integrity >= 0 && state.integrity <= StructureFullIntegrity &&
        (state.brokenGlassFloors & ~StructureGlassMask) == 0 &&
        state.collapsedFromFloor >= 0 && state.collapsedFromFloor <= StructureMaxFloors;
}
// Empty support data is a pre-support save. Aggregate integrity and glass keep
// exactly their old meanings; restoring never invents lost floors or repairs them.
inline bool RestoreStructure(BuildingStructure& out, i32 integrity, u32 glass,
    std::span<const u16> supports, i32 collapsedFromFloor = StructureMaxFloors) {
    if (!supports.empty() && supports.size() != StructureSupportCount) return false;
    BuildingStructure candidate;
    candidate.integrity = integrity;
    candidate.brokenGlassFloors = glass;
    candidate.collapsedFromFloor = collapsedFromFloor;
    if (!ValidStructure(candidate)) return false;
    if (!supports.empty()) std::copy(supports.begin(), supports.end(), candidate.supportDamage.begin());
    out = candidate;
    return true;
}
inline bool ApplyStructuralImpact(BuildingStructure& state, i32 floors, i32 floor,
    i32 support, i32 damage, bool glass, const StructureLimits& limits = {}) {
    if (!ValidStructure(state) || floors < 1 || floors > StructureMaxFloors ||
        floor < 0 || floor >= StandingFloors(state, floors) ||
        support < 0 || support >= StructureSupportsPerFloor || damage <= 0 ||
        limits.supportStrength < 1 || limits.supportStrength > 65535 ||
        limits.requiredSupports < 1 || limits.requiredSupports > StructureSupportsPerFloor) return false;
    if (glass) {
        const u32 flag = 1u << floor;
        if (state.brokenGlassFloors & flag) return false;
        state.brokenGlassFloors |= flag;
        return true;
    }
    damage = std::min(damage, StructureFullIntegrity);
    state.integrity = std::max(0, state.integrity - damage);
    auto& regionalDamage = state.supportDamage[static_cast<std::size_t>(floor * StructureSupportsPerFloor + support)];
    regionalDamage = static_cast<u16>(std::min(65535, i32(regionalDamage) + damage));
    i32 standing = StandingFloors(state, floors);
    for (i32 level = 0; level < standing; ++level) {
        i32 intact = 0;
        for (i32 column = 0; column < StructureSupportsPerFloor; ++column)
            if (state.supportDamage[static_cast<std::size_t>(level * StructureSupportsPerFloor + column)] < limits.supportStrength) ++intact;
        if (intact < limits.requiredSupports) { standing = level; break; }
    }
    state.collapsedFromFloor = std::min(state.collapsedFromFloor, standing);
    return true;
}
inline StructureCondition Condition(const BuildingStructure& state, i32 floors) {
    const i32 standing = StandingFloors(state, floors);
    if (standing == 0 || standing * 2 < floors) return StructureCondition::MajorCollapse;
    if (standing < floors) return StructureCondition::PartialCollapse;
    if (std::any_of(state.supportDamage.begin(), state.supportDamage.end(), [](u16 d) { return d > 0; }))
        return state.integrity <= 90000 ? StructureCondition::Severe : StructureCondition::Damaged;
    return state.integrity < StructureFullIntegrity || state.brokenGlassFloors != 0 ? StructureCondition::Damaged : StructureCondition::Intact;
}
} // namespace lc
