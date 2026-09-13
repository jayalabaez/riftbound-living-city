#include "TestFramework.h"
#include "livingcity/destruct/BuildingStructure.h"
#include <limits>
using namespace lc;

LC_TEST(structure_preserves_legacy_integrity_thresholds_and_glass) {
    for (i32 floors = 3; floors <= 18; ++floors) {
        for (const i32 integrity : {120000, 65001, 65000, 30001, 30000, 1, 0}) {
            BuildingStructure state;
            LC_CHECK(RestoreStructure(state, integrity, 5u, {}));
            LC_CHECK(StandingFloors(state, floors) == LegacyStandingFloors(integrity, floors));
            LC_CHECK(state.brokenGlassFloors == 5u);
        }
    }
}
LC_TEST(structure_glass_does_not_change_support_or_integrity) {
    BuildingStructure state;
    LC_CHECK(ApplyStructuralImpact(state, 9, 1, 0, 3400, true));
    LC_CHECK(!ApplyStructuralImpact(state, 9, 1, 0, 3400, true));
    LC_CHECK(state.integrity == StructureFullIntegrity);
    LC_CHECK(StandingFloors(state, 9) == 9);
    LC_CHECK(Condition(state, 9) == StructureCondition::Damaged);
    for (const auto damage : state.supportDamage) LC_CHECK(damage == 0);
}
LC_TEST(structure_local_support_failure_propagates_upwards_only) {
    BuildingStructure state;
    LC_CHECK(ApplyStructuralImpact(state, 12, 4, 0, 17999, false));
    LC_CHECK(StandingFloors(state, 12) == 12);
    LC_CHECK(ApplyStructuralImpact(state, 12, 4, 0, 1, false));
    LC_CHECK(StandingFloors(state, 12) == 12); // one regional support can fail
    LC_CHECK(ApplyStructuralImpact(state, 12, 4, 1, 18000, false));
    LC_CHECK(state.integrity == 84000); // above the old aggregate-collapse threshold
    LC_CHECK(StandingFloors(state, 12) == 4);
    LC_CHECK(state.collapsedFromFloor == 4);
    LC_CHECK(!ApplyStructuralImpact(state, 12, 4, 2, 100, false));
    LC_CHECK(ApplyStructuralImpact(state, 12, 1, 0, 100, false));
    LC_CHECK(StandingFloors(state, 12) == 4);
}
LC_TEST(structure_ground_support_loss_collapses_all_storeys) {
    BuildingStructure state;
    LC_CHECK(ApplyStructuralImpact(state, 18, 0, 0, 18000, false));
    LC_CHECK(ApplyStructuralImpact(state, 18, 0, 3, 18000, false));
    LC_CHECK(StandingFloors(state, 18) == 0);
    LC_CHECK(Condition(state, 18) == StructureCondition::MajorCollapse);
    LC_CHECK(!ApplyStructuralImpact(state, 18, 0, 1, 1, false));
}
LC_TEST(structure_save_delta_round_trip_continues_identical_damage) {
    BuildingStructure a;
    LC_CHECK(ApplyStructuralImpact(a, 10, 2, 0, 15000, false));
    LC_CHECK(ApplyStructuralImpact(a, 10, 1, 1, 3000, true));
    BuildingStructure b;
    LC_CHECK(RestoreStructure(b, a.integrity, a.brokenGlassFloors, a.supportDamage, a.collapsedFromFloor));
    for (i32 hit = 0; hit < 8; ++hit) {
        const bool resultA = ApplyStructuralImpact(a, 10, 2, hit % 2, 5000, false);
        const bool resultB = ApplyStructuralImpact(b, 10, 2, hit % 2, 5000, false);
        LC_CHECK(resultA == resultB);
        LC_CHECK(a.integrity == b.integrity && a.supportDamage == b.supportDamage);
        LC_CHECK(a.brokenGlassFloors == b.brokenGlassFloors && a.collapsedFromFloor == b.collapsedFromFloor);
    }
    // A later tuning change cannot resurrect a previously collapsed storey.
    StructureLimits strong; strong.supportStrength = 60000;
    LC_CHECK(StandingFloors(b, 10) == 2);
    LC_CHECK(ApplyStructuralImpact(b, 10, 0, 0, 1, false, strong));
    LC_CHECK(StandingFloors(b, 10) == 2);
}
LC_TEST(structure_invalid_inputs_never_mutate_state_or_overflow) {
    BuildingStructure state;
    LC_CHECK(!RestoreStructure(state, -1, 0, {}));
    LC_CHECK(!RestoreStructure(state, 120001, 0, {}));
    LC_CHECK(!RestoreStructure(state, 120000, 1u << 18, {}));
    LC_CHECK(!RestoreStructure(state, 120000, 0, std::array<u16, 1>{0}));
    LC_CHECK(!ApplyStructuralImpact(state, 19, 0, 0, 1, false));
    LC_CHECK(!ApplyStructuralImpact(state, 9, -1, 0, 1, false));
    LC_CHECK(!ApplyStructuralImpact(state, 9, 0, 4, 1, false));
    LC_CHECK(!ApplyStructuralImpact(state, 9, 0, 0, 0, false));
    StructureLimits invalid; invalid.requiredSupports = 0;
    LC_CHECK(!ApplyStructuralImpact(state, 9, 0, 0, 1, false, invalid));
    LC_CHECK(state.integrity == StructureFullIntegrity);
    LC_CHECK(ApplyStructuralImpact(state, 9, 0, 0, std::numeric_limits<i32>::max(), false));
    LC_CHECK(state.integrity == 0 && state.supportDamage[0] == 65535);
}
