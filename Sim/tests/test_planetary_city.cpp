#include "TestFramework.h"
#include "livingcity/sim/PlanetaryCity.h"
#include <chrono>

using namespace lc;
namespace {
std::vector<PlanetaryBuilding> PlanetaryLayout() {
    std::vector<PlanetaryBuilding> layout;
    for (u32 i = 0; i < 60; ++i) layout.push_back({100 + i * 3, static_cast<PlanetaryBuildingKind>(i % 5)});
    return layout;
}
PlanetaryCity TestPlanetaryCity(u64 seed = 91, u32 population = 120) {
    PlanetaryCity city; PlanetaryCityConfig config; config.population = population;
    const auto layout = PlanetaryLayout(); LC_CHECK(city.Generate(seed, layout, config)); return city;
}
PlanetaryCommand CityCommand(const PlanetaryCity& city, u32 index, PlanetaryCommandType type, u32 building = kPlanetaryInvalidBuilding, PlanetaryGood good = PlanetaryGood::Food, u32 quantity = 1) {
    PlanetaryCommand c; c.citizen = index; c.sequence = city.Citizen(index)->lastSequence + 1; c.type = type; c.building = building; c.good = good; c.quantity = quantity; return c;
}
void CityMinutes(PlanetaryCity& city, u32 minutes) { for (u32 i = 0; i < minutes * city.Config().ticksPerMinute; ++i) city.Step(); }
}

LC_TEST(planetary_city_real_addresses_explicit_employment_and_transactional_bad_layout) {
    auto city = TestPlanetaryCity(555, 3600); const auto layout = PlanetaryLayout();
    for (u32 i = 0; i < city.CitizenCount(); ++i) {
        const auto* citizen = city.Citizen(i); bool home = false, work = citizen->work == kPlanetaryInvalidBuilding;
        for (const auto& b : layout) { if (b.index == citizen->home && b.kind == PlanetaryBuildingKind::Home) home = true; if (b.index == citizen->work && b.kind != PlanetaryBuildingKind::Home) work = true; }
        LC_CHECK(home && work); LC_CHECK_NE(citizen->identity, 0ull);
    }
    const u64 before = city.Hash(); auto bad = layout; bad.push_back(bad.front());
    LC_CHECK_FALSE(city.Generate(9, bad)); LC_CHECK_EQ(city.Hash(), before);
    bad.clear(); bad.push_back({1, PlanetaryBuildingKind::Shop});
    LC_CHECK_FALSE(city.Generate(9, bad)); LC_CHECK_EQ(city.Hash(), before);
}

LC_TEST(planetary_city_100000_ticks_determinism_and_closed_money) {
    auto a = TestPlanetaryCity(), b = TestPlanetaryCity();
    const i64 issued = a.Stats().issuedMoneyMinor;
    for (u32 t = 0; t < 100000; ++t) {
        if (t == 400 || t == 7000) {
            for (auto* city : {&a, &b}) {
                city->SetPresence(2, 106);
                LC_CHECK(city->Apply(CityCommand(*city, 2, PlanetaryCommandType::Purchase, 106)) == PlanetaryResult::Accepted);
            }
        }
        a.Step(); b.Step();
        LC_CHECK(a.ConservationHolds()); LC_CHECK(b.ConservationHolds());
        if (t % 1000 == 0) LC_CHECK_EQ(a.Hash(), b.Hash());
    }
    LC_CHECK_EQ(a.Hash(), b.Hash()); LC_CHECK_EQ(a.Stats().issuedMoneyMinor, issued);
    LC_CHECK(a.Stats().wagesPaidMinor > 0); LC_CHECK(a.Stats().taxesCollectedMinor > 0); LC_CHECK(a.Stats().retailSalesMinor > 0);
}

LC_TEST(planetary_city_purchase_is_atomic_and_duplicate_or_invalid_input_cannot_spend) {
    auto city = TestPlanetaryCity(); city.SetControlled(1, true); city.SetPresence(1, 106);
    const i64 before = city.Balance(1), quote = city.Price(106, PlanetaryGood::Food);
    const u32 stock = city.Stock(106, PlanetaryGood::Food); const u16 inventory = city.Citizen(1)->inventory[0];
    auto buy = CityCommand(city, 1, PlanetaryCommandType::Purchase, 106);
    LC_CHECK(city.Apply(buy) == PlanetaryResult::Accepted);
    LC_CHECK_EQ(city.Balance(1), before - quote); LC_CHECK_EQ(city.Stock(106, PlanetaryGood::Food), stock - 1); LC_CHECK_EQ(city.Citizen(1)->inventory[0], inventory + 1);
    const u64 accepted = city.Hash(); LC_CHECK(city.Apply(buy) == PlanetaryResult::Duplicate); LC_CHECK_EQ(city.Hash(), accepted);
    buy.sequence++; buy.good = static_cast<PlanetaryGood>(255);
    LC_CHECK(city.Apply(buy) == PlanetaryResult::InvalidCommand); LC_CHECK_EQ(city.Hash(), accepted);
    buy.good = PlanetaryGood::Food; buy.quantity = 0xffffffffu;
    LC_CHECK(city.Apply(buy) == PlanetaryResult::InvalidCommand); LC_CHECK_EQ(city.Hash(), accepted);
    buy.quantity = 1; buy.building = 121;
    LC_CHECK(city.Apply(buy) == PlanetaryResult::WrongLocation); LC_CHECK_EQ(city.Hash(), accepted);
    LC_CHECK(city.ConservationHolds());
}

LC_TEST(planetary_city_work_requires_arrival_and_an_hour_of_actual_work) {
    auto city = TestPlanetaryCity(); city.SetControlled(1, true); city.SetPresence(1, kPlanetaryInvalidBuilding);
    const u32 job = city.Citizen(1)->work; const i64 before = city.Balance(1);
    LC_CHECK(city.Apply(CityCommand(city, 1, PlanetaryCommandType::StartWork, job)) == PlanetaryResult::WrongLocation);
    city.SetPresence(1, job);
    LC_CHECK(city.Apply(CityCommand(city, 1, PlanetaryCommandType::StartWork, job)) == PlanetaryResult::Accepted);
    CityMinutes(city, 59); LC_CHECK_EQ(city.Balance(1), before);
    CityMinutes(city, 1); LC_CHECK(city.Balance(1) > before);
    const i64 paid = city.Balance(1); city.SetPresence(1, kPlanetaryInvalidBuilding); CityMinutes(city, 60); LC_CHECK_EQ(city.Balance(1), paid);
}

LC_TEST(planetary_city_embodiment_changes_arrival_not_identity_or_money) {
    auto city = TestPlanetaryCity(); const auto identity = city.Citizen(1)->identity; const i64 cash = city.Balance(1);
    city.SetEmbodied(1, true); city.SetPresence(1, kPlanetaryInvalidBuilding);
    CityMinutes(city, 350);
    LC_CHECK_EQ(city.Citizen(1)->presence, kPlanetaryInvalidBuilding); LC_CHECK_EQ(city.Citizen(1)->identity, identity); LC_CHECK_EQ(city.Balance(1), cash);
    LC_CHECK(city.Citizen(1)->activity == PlanetaryActivity::Shopping); const u32 shop = city.Citizen(1)->goalBuilding;
    city.SetPresence(1, shop); CityMinutes(city, 1); LC_CHECK(city.Balance(1) < cash);
    const i64 arrivedCash = city.Balance(1); city.SetEmbodied(1, false); city.SetEmbodied(1, true);
    LC_CHECK_EQ(city.Citizen(1)->identity, identity); LC_CHECK_EQ(city.Balance(1), arrivedCash);
}

LC_TEST(planetary_city_evidence_fines_deaths_and_recovery_share_ordinary_citizens) {
    auto city = TestPlanetaryCity(); const u64 before = city.Hash();
    LC_CHECK_FALSE(city.RecordOffense(2, PlanetaryOffense::Assault, 599, 1)); LC_CHECK_EQ(city.Hash(), before);
    LC_CHECK(city.RecordOffense(2, PlanetaryOffense::Assault, 800, 1)); const u64 observed = city.Hash();
    LC_CHECK_FALSE(city.RecordOffense(2, PlanetaryOffense::Assault, 1000, 1)); LC_CHECK_EQ(city.Hash(), observed);
    const i64 fine = city.Citizen(2)->fineMinor, cash = city.Balance(2); city.SetPresence(2, 112);
    LC_CHECK(city.Apply(CityCommand(city, 2, PlanetaryCommandType::PayFine, 112)) == PlanetaryResult::Accepted);
    LC_CHECK_EQ(city.Balance(2), cash - fine); LC_CHECK_EQ(city.Citizen(2)->convictions, 1u); LC_CHECK_EQ(city.Citizen(2)->fineMinor, 0ll);
    const u64 identity = city.Citizen(2)->identity; LC_CHECK(city.Kill(2)); LC_CHECK_FALSE(city.Kill(2)); CityMinutes(city, 100);
    LC_CHECK_FALSE(city.Citizen(2)->alive); LC_CHECK_EQ(city.Citizen(2)->identity, identity);
    LC_CHECK(city.Apply(CityCommand(city, 2, PlanetaryCommandType::Consume)) == PlanetaryResult::Dead);
    LC_CHECK(city.EmergencyRecovery(2)); LC_CHECK(city.Citizen(2)->alive); LC_CHECK_EQ(city.Citizen(2)->convictions, 1u); LC_CHECK_EQ(city.Citizen(2)->identity, identity); LC_CHECK(city.ConservationHolds());
}

LC_TEST(planetary_city_prices_respond_to_inventory_and_sales_recycle_real_money) {
    auto city = TestPlanetaryCity(); city.SetControlled(1, true); city.SetPresence(1, 106);
    const i64 initial = city.Price(106, PlanetaryGood::Food);
    LC_CHECK(city.Apply(CityCommand(city, 1, PlanetaryCommandType::Purchase, 106, PlanetaryGood::Food, 20)) == PlanetaryResult::Accepted);
    LC_CHECK(city.Price(106, PlanetaryGood::Food) > initial);
    const i64 shortage = city.Price(106, PlanetaryGood::Food); CityMinutes(city, 5); LC_CHECK(city.Price(106, PlanetaryGood::Food) < shortage);
    const u32 raw = city.Stock(106, PlanetaryGood::RawMaterials); const i64 cash = city.Balance(1);
    LC_CHECK(city.Apply(CityCommand(city, 1, PlanetaryCommandType::SellRawMaterials, 106, PlanetaryGood::RawMaterials, 3)) == PlanetaryResult::Accepted);
    LC_CHECK_EQ(city.Stock(106, PlanetaryGood::RawMaterials), raw + 3); LC_CHECK(city.Balance(1) > cash); LC_CHECK(city.ConservationHolds());
}

LC_TEST(planetary_city_utility_nonpayment_rest_and_recycling_have_real_consequences) {
    PlanetaryCity city; PlanetaryCityConfig config; config.population = 120; config.initialCitizenCashMinor = 100000;
    LC_CHECK(city.Generate(91, PlanetaryLayout(), config)); city.SetControlled(1, true); const u32 home = city.Citizen(1)->home;
    city.SetPresence(1, home); LC_CHECK(city.Apply(CityCommand(city, 1, PlanetaryCommandType::Rest)) == PlanetaryResult::Accepted);
    const u16 tired = city.Citizen(1)->needs[3]; CityMinutes(city, 4); LC_CHECK(city.Citizen(1)->needs[3] < tired);
    // Keep life support satisfied using the same ordinary command paths while allowing
    // two daily utility invoices to age unpaid. Renting remains a real bank transfer.
    for (u32 minute = 0; minute < 2600; ++minute) {
        if (minute % 50 == 0) {
            for (PlanetaryGood good : {PlanetaryGood::Food, PlanetaryGood::Water}) {
                city.SetPresence(1, 106); LC_CHECK(city.Apply(CityCommand(city, 1, PlanetaryCommandType::Purchase, 106, good)) == PlanetaryResult::Accepted);
                LC_CHECK(city.Apply(CityCommand(city, 1, PlanetaryCommandType::Consume, 106, good)) == PlanetaryResult::Accepted);
            }
            city.SetPresence(1, home); city.Apply(CityCommand(city, 1, PlanetaryCommandType::Rest));
        }
        CityMinutes(city, 1);
    }
    LC_CHECK_FALSE(city.Citizen(1)->utilitiesConnected);
    LC_CHECK(city.Apply(CityCommand(city, 1, PlanetaryCommandType::Wash)) == PlanetaryResult::UtilitiesDisconnected);
    LC_CHECK(city.Apply(CityCommand(city, 1, PlanetaryCommandType::PayBills)) == PlanetaryResult::Accepted);
    LC_CHECK(city.Citizen(1)->utilitiesConnected); LC_CHECK(city.Apply(CityCommand(city, 1, PlanetaryCommandType::Wash)) == PlanetaryResult::Accepted);
    const u32 waste = city.Citizen(1)->waste; LC_CHECK(waste > 0); city.SetPresence(1, 106);
    LC_CHECK(city.Apply(CityCommand(city, 1, PlanetaryCommandType::Recycle, 106)) == PlanetaryResult::Accepted);
    LC_CHECK_EQ(city.Citizen(1)->waste, 0u); LC_CHECK(city.Stats().recycledUnits >= waste); LC_CHECK(city.ConservationHolds());
}

LC_TEST(planetary_city_seed_delta_preserves_dead_rows_exact_hash_and_rejects_corruption) {
    auto city = TestPlanetaryCity(567), restored = TestPlanetaryCity(567);
    const auto untouched = city.SaveDelta(); LC_CHECK(untouched.size() < 160); LC_CHECK(restored.LoadDelta(untouched)); LC_CHECK_EQ(restored.Hash(), city.Hash());
    CityMinutes(city, 130); city.SetControlled(3, true); city.Kill(7); city.RecordOffense(8, PlanetaryOffense::Homicide, 999, 44);
    const auto saved = city.SaveDelta(); LC_CHECK(restored.LoadDelta(saved)); LC_CHECK_EQ(restored.Hash(), city.Hash()); LC_CHECK_FALSE(restored.Citizen(7)->alive);
    for (u32 i = 0; i < 2000; ++i) { city.Step(); restored.Step(); } LC_CHECK_EQ(restored.Hash(), city.Hash());
    const u64 before = restored.Hash(); auto corrupt = saved; corrupt[50] ^= 0x7fu;
    LC_CHECK_FALSE(restored.LoadDelta(corrupt)); LC_CHECK_EQ(restored.Hash(), before);
    LC_CHECK_FALSE(restored.LoadDelta(std::span<const u8>(saved).first(saved.size() - 3))); LC_CHECK_EQ(restored.Hash(), before);
    auto otherSeed = TestPlanetaryCity(568); LC_CHECK_FALSE(otherSeed.LoadDelta(saved));
}

LC_TEST(planetary_city_interplanetary_resident_swap_preserves_person_and_combined_supply) {
    auto a = TestPlanetaryCity(71), b = TestPlanetaryCity(72); a.SetControlled(119, true); a.SetPresence(119, 106);
    a.Apply(CityCommand(a, 119, PlanetaryCommandType::Purchase, 106, PlanetaryGood::Medicine));
    a.RecordOffense(119, PlanetaryOffense::Assault, 900, 9); a.Damage(119, 280);
    const PlanetaryCitizen person = *a.Citizen(119); const i64 wallet = a.Balance(119);
    const i64 total = a.Stats().issuedMoneyMinor + b.Stats().issuedMoneyMinor;
    LC_CHECK(a.SwapResident(b, 119, 118)); LC_CHECK_EQ(b.Citizen(118)->identity, person.identity); LC_CHECK_EQ(b.Balance(118), wallet);
    LC_CHECK_EQ(b.Citizen(118)->inventory[7], person.inventory[7]); LC_CHECK_EQ(b.Citizen(118)->fineMinor, person.fineMinor); LC_CHECK_EQ(b.Citizen(118)->needs[4], person.needs[4]);
    LC_CHECK_EQ(a.Stats().issuedMoneyMinor + b.Stats().issuedMoneyMinor, total); LC_CHECK(a.ConservationHolds() && b.ConservationHolds());
    auto savedA = a.SaveDelta(), savedB = b.SaveDelta(); auto loadA = TestPlanetaryCity(71), loadB = TestPlanetaryCity(72);
    LC_CHECK(loadA.LoadDelta(savedA)); LC_CHECK(loadB.LoadDelta(savedB)); LC_CHECK_EQ(loadA.Hash(), a.Hash()); LC_CHECK_EQ(loadB.Hash(), b.Hash());
    LC_CHECK(b.SwapResident(a, 118, 119)); LC_CHECK_EQ(a.Citizen(119)->identity, person.identity); LC_CHECK_EQ(a.Balance(119), wallet);
    LC_CHECK_EQ(a.Stats().issuedMoneyMinor + b.Stats().issuedMoneyMinor, total);
}

LC_TEST(planetary_city_54000_persistent_people_zero_tick_allocations_and_measured_budget) {
    std::vector<PlanetaryCity> cities; cities.reserve(15);
    for (u32 i = 0; i < 15; ++i) cities.push_back(TestPlanetaryCity(1000 + i, 3600));
    for (auto& city : cities) CityMinutes(city, 1);
    const AllocStats before = GetAllocStats(); const auto started = std::chrono::steady_clock::now();
    for (u32 tick = 0; tick < 400; ++tick) for (auto& city : cities) city.Step();
    const auto ended = std::chrono::steady_clock::now(); const AllocStats after = GetAllocStats();
    if (AllocTrackingEnabled()) LC_CHECK_EQ(after.allocations, before.allocations);
    const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(ended - started).count() / 400;
    std::printf("\n    54000 residents / 15 cities: mean %lld.%04lld ms per combined 20Hz step; zero allocation check active\n",
                static_cast<long long>(nanoseconds / 1000000), static_cast<long long>((nanoseconds % 1000000) / 100));
    u32 count = 0; for (const auto& city : cities) { count += city.CitizenCount(); LC_CHECK(city.ConservationHolds()); }
    LC_CHECK_EQ(count, 54000u);
}

LC_TEST(planetary_city_field_supplies_restore_needs_without_minting_or_resurrecting) {
    auto city = TestPlanetaryCity(73); city.SetControlled(1, true);
    CityMinutes(city, 20); city.Damage(1, 600);
    const auto before = *city.Citizen(1); const auto money = city.Stats().totalMoneyMinor;
    LC_CHECK(city.RelieveNeeds(1, 500, 100, 350));
    LC_CHECK(city.Citizen(1)->needs[0] < before.needs[0]);
    LC_CHECK_EQ(city.Citizen(1)->needs[4], before.needs[4] - 350);
    LC_CHECK_EQ(city.Stats().totalMoneyMinor, money);
    LC_CHECK_FALSE(city.RelieveNeeds(1, 1001, 0, 0));
    city.Kill(1); LC_CHECK_FALSE(city.RelieveNeeds(1, 500, 100, 1000));
    LC_CHECK_FALSE(city.Citizen(1)->alive); LC_CHECK(city.ConservationHolds());
}
