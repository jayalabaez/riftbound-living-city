// Persistent city economy for planetary settlements. The host supplies real building
// indices and observed inputs; this module owns deterministic identities and accounts.
#pragma once
#include "livingcity/core/Money.h"
#include <array>
#include <span>
#include <vector>

namespace lc {
constexpr u32 kPlanetaryInvalidBuilding = 0xffffffffu;
constexpr u32 kPlanetaryGoodCount = 8;
constexpr u32 kPlanetaryNeedCount = 9;
enum class PlanetaryBuildingKind : u8 { Home, Work, Shop, Clinic, Civic };
enum class PlanetaryGood : u8 { Food, Water, Fuel, RawMaterials, BuildingMaterials, ConsumerGoods, Electronics, Medicine };
// All nine values are pressures: zero satisfied/healthy, 1000 critical/dead.
enum class PlanetaryNeed : u8 { Hunger, Thirst, Hygiene, Rest, Health, Energy, Social, Safety, Comfort };
enum class PlanetaryActivity : u8 { Home, Working, Shopping, Resting, Socializing, SeekingCare, Dead };
enum class PlanetaryCommandType : u8 { Purchase, Consume, ApplyJob, StartWork, StopWork, Rest, PayFine, RentHome, PayBills, Wash, Recycle, SellRawMaterials };
enum class PlanetaryResult : u8 { Accepted, InvalidCitizen, Dead, Duplicate, InvalidCommand, WrongLocation, InsufficientFunds, OutOfStock, NoInventory, NoJob, UtilitiesDisconnected };
enum class PlanetaryOffense : u8 { Assault, Homicide, PropertyDamage, PatrolAttack };

struct PlanetaryBuilding {
    u32 index = kPlanetaryInvalidBuilding;
    PlanetaryBuildingKind kind = PlanetaryBuildingKind::Home;
};
struct PlanetaryCityConfig {
    u32 population = 3600;
    u32 ticksPerMinute = 20; // One game minute per real second at fixed 20 Hz.
    u32 startMinute = 8 * 60;
    std::array<i64, kPlanetaryGoodCount> basePricesMinor = {500, 150, 800, 350, 1200, 900, 3500, 1800};
    i64 initialCitizenCashMinor = 20000;
    i64 initialFirmCapitalMinor = 5000000;
    i64 governmentReserveMinor = 1000000000;
    i64 hourlyWageMinor = 1800;
    i64 dailyRentMinor = 900;
    i64 dailyUtilitiesMinor = 180;
    i32 salesTaxBasisPoints = 1000;
    i32 incomeTaxBasisPoints = 1000;
};
struct PlanetaryCitizen {
    u64 identity = 0;
    u32 ordinal = 0;
    u32 home = kPlanetaryInvalidBuilding;
    u32 work = kPlanetaryInvalidBuilding; // Invalid means explicitly unemployed.
    u32 presence = kPlanetaryInvalidBuilding;
    u32 goalBuilding = kPlanetaryInvalidBuilding;
    u16 shiftStartMinute = 8 * 60;
    u16 shiftEndMinute = 17 * 60;
    PlanetaryActivity activity = PlanetaryActivity::Home;
    std::array<u16, kPlanetaryNeedCount> needs{};
    std::array<u16, kPlanetaryGoodCount> inventory{};
    i64 fineMinor = 0;
    i64 billsDueMinor = 0;
    u32 convictions = 0;
    u32 waste = 0;
    u32 workMinutes = 0;
    u64 lastSequence = 0;
    u64 lastEvidenceId = 0;
    bool alive = true;
    bool controlled = false; // Any external controller suppresses autonomous actions.
    bool embodied = false; // Arrival is observed by host; abstract travel cannot teleport it.
    bool utilitiesConnected = true;
};
struct PlanetaryCommand {
    u32 citizen = 0;
    u64 sequence = 0; // Strictly increasing, nonzero, per citizen. Accepted commands only.
    PlanetaryCommandType type = PlanetaryCommandType::Purchase;
    u32 building = kPlanetaryInvalidBuilding;
    PlanetaryGood good = PlanetaryGood::Food;
    u32 quantity = 1;
};
struct PlanetaryCityStats {
    u32 population = 0;
    u32 living = 0;
    u32 unemployed = 0;
    u32 hungry = 0;
    u32 disconnected = 0;
    u32 waste = 0;
    u32 recycledUnits = 0;
    u32 minuteOfDay = 0;
    u64 tick = 0;
    i64 totalMoneyMinor = 0;
    i64 issuedMoneyMinor = 0;
    i64 governmentMinor = 0;
    i64 wagesPaidMinor = 0;
    i64 retailSalesMinor = 0;
    i64 taxesCollectedMinor = 0;
    i64 finesCollectedMinor = 0;
};

class LC_API PlanetaryCity {
public:
    // Rejects layouts missing real homes, workplaces or shops, and duplicate indices.
    bool Generate(u64 seed, std::span<const PlanetaryBuilding> buildings, const PlanetaryCityConfig& config = {});
    void Step(); // Exactly one 20 Hz step; no IO, allocations or floating-point state.
    PlanetaryResult Apply(const PlanetaryCommand& command);
    bool SetControlled(u32 citizen, bool controlled);
    bool SetEmbodied(u32 citizen, bool embodied);
    bool SetPresence(u32 citizen, u32 building);
    // Moves ordinary resident identities and possessions between reserved resident slots.
    // Paired central-bank clearing conserves the combined supply exactly. Addresses and
    // employment are reassigned to real local buildings, without granting fresh money.
    bool SwapResident(PlanetaryCity& destination, u32 citizen, u32 destinationCitizen);
    bool RecordOffense(u32 citizen, PlanetaryOffense offense, u16 evidenceConfidence, u64 eventId);
    bool Damage(u32 citizen, u16 pressure);
    // Ordered host evidence of an already consumed field ration or medical supply.
    bool RelieveNeeds(u32 citizen, u16 food, u16 water, u16 health);
    bool Kill(u32 citizen);
    bool EmergencyRecovery(u32 citizen);

    const PlanetaryCitizen* Citizen(u32 citizen) const;
    i64 Balance(u32 citizen) const;
    u32 Stock(u32 building, PlanetaryGood good) const;
    i64 Price(u32 building, PlanetaryGood good) const; // Includes sales tax.
    PlanetaryCityStats Stats() const;
    u64 Seed() const { return seed_; }
    u64 Tick() const { return tick_; }
    u32 MinuteOfDay() const;
    u32 CitizenCount() const { return static_cast<u32>(citizens_.size()); }
    const PlanetaryCityConfig& Config() const { return config_; }
    bool ConservationHolds() const { return ledger_.ConservationHolds(); }
    u64 Hash() const;
    // IO belongs to host. Seed/config/layout are regenerated and only divergent records
    // are encoded; corrupt, incompatible or unbalanced input leaves this city untouched.
    std::vector<u8> SaveDelta() const;
    bool LoadDelta(std::span<const u8> bytes);

private:
    struct Firm {
        PlanetaryBuilding building;
        AccountId account;
        std::array<u32, kPlanetaryGoodCount> stock{};
        std::array<i64, kPlanetaryGoodCount> price{};
        u32 targetStock = 0;
    };
    PlanetaryCityConfig config_;
    u64 seed_ = 0;
    u64 tick_ = 0;
    Ledger ledger_;
    std::vector<PlanetaryCitizen> citizens_;
    std::vector<AccountId> accounts_;
    std::vector<Firm> firms_;
    std::vector<u32> homes_, workplaces_, shops_;
    i64 wagesPaidMinor_ = 0, retailSalesMinor_ = 0, taxesCollectedMinor_ = 0, finesCollectedMinor_ = 0;
    u32 recycledUnits_ = 0;
    const Firm* FindFirm(u32 building) const;
    Firm* FindFirm(u32 building);
    PlanetaryCitizen InitialCitizen(u32 ordinal) const;
    void Minute(u32 citizen);
    void Market();
    void Reprice(Firm& firm);
    bool Purchase(u32 citizen, Firm& firm, u32 good, u32 quantity);
    void Consume(PlanetaryCitizen& citizen, u32 good);
    void HashInto(Hasher& hash) const;
};
} // namespace lc
