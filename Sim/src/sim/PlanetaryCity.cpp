#include "livingcity/sim/PlanetaryCity.h"
#include <algorithm>
#include <limits>
#include <utility>

namespace lc {
namespace planetary_detail {
constexpr u32 N(PlanetaryNeed n) { return static_cast<u32>(n); }
constexpr u32 G(PlanetaryGood g) { return static_cast<u32>(g); }
u64 Mix(u64 v) {
    v += 0x9e3779b97f4a7c15ull;
    v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ull;
    v = (v ^ (v >> 27)) * 0x94d049bb133111ebull;
    return v ^ (v >> 31);
}
u16 Raise(u16 v, u32 d) { return static_cast<u16>(std::min<u32>(1000, v + d)); }
u16 Lower(u16 v, u32 d) { return static_cast<u16>(v > d ? v - d : 0); }
bool ValidConfig(const PlanetaryCityConfig& c) {
    if (c.population == 0 || c.population > 100000 || c.ticksPerMinute == 0 || c.ticksPerMinute > 1200 || c.startMinute >= 1440) return false;
    if (c.initialCitizenCashMinor < 0 || c.initialCitizenCashMinor > 1000000000 || c.initialFirmCapitalMinor < 0 || c.initialFirmCapitalMinor > 1000000000 || c.governmentReserveMinor < 0 || c.governmentReserveMinor > 1000000000000ll) return false;
    if (c.hourlyWageMinor < 1 || c.hourlyWageMinor > 1000000 || c.dailyRentMinor < 0 || c.dailyRentMinor > 1000000 || c.dailyUtilitiesMinor < 0 || c.dailyUtilitiesMinor > 1000000) return false;
    if (c.salesTaxBasisPoints < 0 || c.salesTaxBasisPoints > 10000 || c.incomeTaxBasisPoints < 0 || c.incomeTaxBasisPoints > 10000) return false;
    for (i64 price : c.basePricesMinor) if (price < 1 || price > 1000000) return false;
    return true;
}
void HashConfig(Hasher& h, const PlanetaryCityConfig& c) {
    h.U32(c.population); h.U32(c.ticksPerMinute); h.U32(c.startMinute);
    for (i64 p : c.basePricesMinor) h.I64(p);
    h.I64(c.initialCitizenCashMinor); h.I64(c.initialFirmCapitalMinor); h.I64(c.governmentReserveMinor);
    h.I64(c.hourlyWageMinor); h.I64(c.dailyRentMinor); h.I64(c.dailyUtilitiesMinor);
    h.I32(c.salesTaxBasisPoints); h.I32(c.incomeTaxBasisPoints);
}
void HashCitizen(Hasher& h, const PlanetaryCitizen& c) {
    h.U64(c.identity); h.U32(c.ordinal); h.U32(c.home); h.U32(c.work); h.U32(c.presence); h.U32(c.goalBuilding); h.U32(c.shiftStartMinute); h.U32(c.shiftEndMinute); h.U8(static_cast<u8>(c.activity));
    for (u16 n : c.needs) h.U32(n);
    for (u16 i : c.inventory) h.U32(i);
    h.I64(c.fineMinor); h.I64(c.billsDueMinor); h.U32(c.convictions); h.U32(c.waste); h.U32(c.workMinutes);
    h.U64(c.lastSequence); h.U64(c.lastEvidenceId); h.U8(c.alive ? 1u : 0u); h.U8(c.controlled ? 1u : 0u); h.U8(c.embodied ? 1u : 0u); h.U8(c.utilitiesConnected ? 1u : 0u);
}
bool SameCitizen(const PlanetaryCitizen& a, const PlanetaryCitizen& b) { Hasher ha, hb; HashCitizen(ha, a); HashCitizen(hb, b); return ha.Value() == hb.Value(); }
struct Writer {
    std::vector<u8> bytes;
    void U(u64 v, u32 n = 8) { for (u32 i = 0; i < n; ++i) bytes.push_back(static_cast<u8>(v >> (i * 8))); }
    void Citizen(const PlanetaryCitizen& c) {
        U(c.identity); U(c.home, 4); U(c.work, 4); U(c.presence, 4); U(c.goalBuilding, 4); U(c.shiftStartMinute, 2); U(c.shiftEndMinute, 2); U(static_cast<u8>(c.activity), 1);
        for (u16 n : c.needs) U(n, 2);
        for (u16 i : c.inventory) U(i, 2);
        U(static_cast<u64>(c.fineMinor)); U(static_cast<u64>(c.billsDueMinor)); U(c.convictions, 4); U(c.waste, 4); U(c.workMinutes, 4);
        U(c.lastSequence); U(c.lastEvidenceId); U(c.alive ? 1u : 0u, 1); U(c.controlled ? 1u : 0u, 1); U(c.embodied ? 1u : 0u, 1); U(c.utilitiesConnected ? 1u : 0u, 1);
    }
};
struct Reader {
    std::span<const u8> bytes;
    std::size_t pos = 0;
    bool good = true;
    u64 U(u32 n = 8) {
        if (!good || n > bytes.size() - pos) { good = false; return 0; }
        u64 v = 0; for (u32 i = 0; i < n; ++i) v |= static_cast<u64>(bytes[pos++]) << (i * 8);
        return v;
    }
    void Citizen(PlanetaryCitizen& c) {
        c.identity = U(); c.home = static_cast<u32>(U(4)); c.work = static_cast<u32>(U(4)); c.presence = static_cast<u32>(U(4)); c.goalBuilding = static_cast<u32>(U(4)); c.shiftStartMinute = static_cast<u16>(U(2)); c.shiftEndMinute = static_cast<u16>(U(2)); c.activity = static_cast<PlanetaryActivity>(U(1));
        for (u16& n : c.needs) n = static_cast<u16>(U(2));
        for (u16& i : c.inventory) i = static_cast<u16>(U(2));
        c.fineMinor = static_cast<i64>(U()); c.billsDueMinor = static_cast<i64>(U()); c.convictions = static_cast<u32>(U(4)); c.waste = static_cast<u32>(U(4)); c.workMinutes = static_cast<u32>(U(4));
        c.lastSequence = U(); c.lastEvidenceId = U();
        const u64 alive = U(1), controlled = U(1), embodied = U(1), connected = U(1);
        if (alive > 1 || controlled > 1 || embodied > 1 || connected > 1) good = false;
        c.alive = alive != 0; c.controlled = controlled != 0; c.embodied = embodied != 0; c.utilitiesConnected = connected != 0;
    }
};
} // namespace planetary_detail

bool PlanetaryCity::Generate(u64 seed, std::span<const PlanetaryBuilding> buildings, const PlanetaryCityConfig& config) {
    if (!planetary_detail::ValidConfig(config) || buildings.empty() || buildings.size() > 4096) return false;
    PlanetaryCity next;
    next.seed_ = seed; next.config_ = config;
    for (const auto& b : buildings) {
        if (b.index == kPlanetaryInvalidBuilding || static_cast<u8>(b.kind) > static_cast<u8>(PlanetaryBuildingKind::Civic)) return false;
        Firm f; f.building = b; next.firms_.push_back(f);
    }
    std::sort(next.firms_.begin(), next.firms_.end(), [](const Firm& a, const Firm& b) { return a.building.index < b.building.index; });
    for (u32 i = 0; i < next.firms_.size(); ++i) {
        const auto& b = next.firms_[i].building;
        if (i > 0 && b.index == next.firms_[i - 1].building.index) return false;
        if (b.kind == PlanetaryBuildingKind::Home) next.homes_.push_back(b.index);
        else next.workplaces_.push_back(b.index);
        if (b.kind == PlanetaryBuildingKind::Shop) next.shops_.push_back(b.index);
    }
    if (next.homes_.empty() || next.workplaces_.empty() || next.shops_.empty()) return false;
    next.ledger_.Reserve(config.population + static_cast<u32>(buildings.size()) + 1);
    const AccountId bank = next.ledger_.OpenAccount(); next.ledger_.SetCentralBank(bank);
    next.ledger_.Mint(Money(config.governmentReserveMinor));
    for (auto& f : next.firms_) {
        f.account = next.ledger_.OpenAccount(Money(config.initialFirmCapitalMinor));
        f.targetStock = std::max<u32>(64, config.population / static_cast<u32>(next.shops_.size()));
        if (f.building.kind == PlanetaryBuildingKind::Shop) f.stock.fill(f.targetStock);
        next.Reprice(f);
    }
    next.citizens_.reserve(config.population); next.accounts_.reserve(config.population);
    for (u32 i = 0; i < config.population; ++i) {
        next.citizens_.push_back(next.InitialCitizen(i));
        next.accounts_.push_back(next.ledger_.OpenAccount(Money(config.initialCitizenCashMinor)));
    }
    *this = std::move(next);
    return true;
}
PlanetaryCitizen PlanetaryCity::InitialCitizen(u32 ordinal) const {
    PlanetaryCitizen c;
    c.ordinal = ordinal; c.identity = planetary_detail::Mix(seed_ ^ (static_cast<u64>(ordinal) << 32));
    if (c.identity == 0) c.identity = 1;
    c.home = homes_[static_cast<std::size_t>(planetary_detail::Mix(seed_ + ordinal)) % homes_.size()];
    c.work = ordinal % 10 == 0 ? kPlanetaryInvalidBuilding : workplaces_[ordinal % workplaces_.size()];
    c.presence = c.home; c.goalBuilding = c.home;
    c.shiftStartMinute = static_cast<u16>(6 * 60 + (ordinal % 4) * 60); c.shiftEndMinute = static_cast<u16>(c.shiftStartMinute + 9 * 60);
    for (u32 i = 0; i < kPlanetaryNeedCount; ++i) c.needs[i] = static_cast<u16>(planetary_detail::Mix(c.identity + i) % 120);
    c.needs[planetary_detail::N(PlanetaryNeed::Health)] = 0;
    c.inventory[0] = 1; c.inventory[1] = 2;
    return c;
}
const PlanetaryCity::Firm* PlanetaryCity::FindFirm(u32 building) const {
    const auto it = std::lower_bound(firms_.begin(), firms_.end(), building, [](const Firm& f, u32 b) { return f.building.index < b; });
    return it != firms_.end() && it->building.index == building ? &*it : nullptr;
}
PlanetaryCity::Firm* PlanetaryCity::FindFirm(u32 building) { return const_cast<Firm*>(std::as_const(*this).FindFirm(building)); }
const PlanetaryCitizen* PlanetaryCity::Citizen(u32 citizen) const { return citizen < citizens_.size() ? &citizens_[citizen] : nullptr; }
i64 PlanetaryCity::Balance(u32 citizen) const { return citizen < accounts_.size() ? ledger_.BalanceOf(accounts_[citizen]).Raw() : 0; }
u32 PlanetaryCity::Stock(u32 building, PlanetaryGood good) const {
    const Firm* f = FindFirm(building); const u32 g = static_cast<u32>(good);
    return f && g < kPlanetaryGoodCount && f->building.kind == PlanetaryBuildingKind::Shop ? f->stock[g] : 0;
}
i64 PlanetaryCity::Price(u32 building, PlanetaryGood good) const {
    const Firm* f = FindFirm(building); const u32 g = static_cast<u32>(good);
    if (!f || g >= kPlanetaryGoodCount || f->building.kind != PlanetaryBuildingKind::Shop) return 0;
    return f->price[g] + Money(f->price[g]).ApplyRate(config_.salesTaxBasisPoints).Raw();
}
u32 PlanetaryCity::MinuteOfDay() const { return (config_.startMinute + static_cast<u32>((tick_ / config_.ticksPerMinute) % 1440)) % 1440; }
bool PlanetaryCity::SetControlled(u32 citizen, bool controlled) {
    if (citizen >= citizens_.size()) return false;
    citizens_[citizen].controlled = controlled; return true;
}
bool PlanetaryCity::SetEmbodied(u32 citizen, bool embodied) {
    if (citizen >= citizens_.size()) return false;
    citizens_[citizen].embodied = embodied; return true;
}
bool PlanetaryCity::SwapResident(PlanetaryCity& destination, u32 citizen, u32 destinationCitizen) {
    if (&destination == this || citizen >= citizens_.size() || destinationCitizen >= destination.citizens_.size() || !citizens_[citizen].alive || !destination.citizens_[destinationCitizen].alive) return false;
    const i64 aBalance = Balance(citizen), bBalance = destination.Balance(destinationCitizen);
    const i64 difference = bBalance - aBalance;
    PlanetaryCity* receiving = difference > 0 ? this : &destination;
    PlanetaryCity* sending = difference > 0 ? &destination : this;
    const u32 receiver = difference > 0 ? citizen : destinationCitizen;
    const u32 sender = difference > 0 ? destinationCitizen : citizen;
    const i64 amount = difference >= 0 ? difference : -difference;
    if (amount > 0) {
        if (receiving->ledger_.TotalIssued().Raw() > std::numeric_limits<i64>::max() - amount) return false;
        // A cross-bank settlement: the same minor units leave one issued supply and
        // enter the other. The two cities together never gain or lose money.
        const bool debited = sending->ledger_.Transfer(sending->accounts_[sender], sending->ledger_.CentralBank(), Money(amount)); LC_ASSERT(debited);
        const bool burned = sending->ledger_.Burn(Money(amount)); LC_ASSERT(burned);
        const bool minted = receiving->ledger_.Mint(Money(amount)); LC_ASSERT(minted);
        const bool credited = receiving->ledger_.Transfer(receiving->ledger_.CentralBank(), receiving->accounts_[receiver], Money(amount)); LC_ASSERT(credited);
    }
    auto& a = citizens_[citizen]; auto& b = destination.citizens_[destinationCitizen];
    const u32 aHome = a.home, aWork = a.work, bHome = b.home, bWork = b.work;
    std::swap(a, b);
    a.ordinal = citizen; a.home = aHome; a.work = aWork; a.presence = aHome; a.goalBuilding = aHome; a.activity = PlanetaryActivity::Home; a.workMinutes = 0;
    b.ordinal = destinationCitizen; b.home = bHome; b.work = bWork; b.presence = bHome; b.goalBuilding = bHome; b.activity = PlanetaryActivity::Home; b.workMinutes = 0;
    return true;
}
bool PlanetaryCity::SetPresence(u32 citizen, u32 building) {
    if (citizen >= citizens_.size() || (!FindFirm(building) && building != kPlanetaryInvalidBuilding)) return false;
    auto& c = citizens_[citizen]; c.presence = building;
    if (c.alive && c.activity == PlanetaryActivity::Working && building != c.work) c.activity = PlanetaryActivity::Home;
    if (c.alive && c.activity == PlanetaryActivity::Resting && building != c.home) c.activity = PlanetaryActivity::Home;
    return true;
}
bool PlanetaryCity::Damage(u32 citizen, u16 pressure) {
    if (citizen >= citizens_.size() || !citizens_[citizen].alive || pressure == 0) return false;
    auto& c = citizens_[citizen]; auto& health = c.needs[planetary_detail::N(PlanetaryNeed::Health)];
    health = planetary_detail::Raise(health, pressure);
    if (health == 1000) return Kill(citizen);
    return true;
}
bool PlanetaryCity::Kill(u32 citizen) {
    if (citizen >= citizens_.size() || !citizens_[citizen].alive) return false;
    auto& c = citizens_[citizen]; c.alive = false; c.activity = PlanetaryActivity::Dead;
    c.needs[planetary_detail::N(PlanetaryNeed::Health)] = 1000; c.workMinutes = 0;
    return true;
}
bool PlanetaryCity::EmergencyRecovery(u32 citizen) {
    if (citizen >= citizens_.size()) return false;
    auto& c = citizens_[citizen];
    c.alive = true; c.needs[planetary_detail::N(PlanetaryNeed::Health)] = 0;
    for (u16& pressure : c.needs) pressure = std::min<u16>(pressure, 400);
    c.activity = PlanetaryActivity::Home; c.presence = c.home; c.goalBuilding = c.home; c.workMinutes = 0;
    const i64 fee = std::min<i64>(1000, Balance(citizen));
    ledger_.Transfer(accounts_[citizen], ledger_.CentralBank(), Money(fee));
    return true;
}
bool PlanetaryCity::RecordOffense(u32 citizen, PlanetaryOffense offense, u16 evidenceConfidence, u64 eventId) {
    if (citizen >= citizens_.size() || evidenceConfidence < 600 || evidenceConfidence > 1000 || eventId == 0 || static_cast<u8>(offense) > 3) return false;
    auto& c = citizens_[citizen];
    if (!c.alive || eventId <= c.lastEvidenceId) return false;
    constexpr std::array<i64, 4> fines = {5000, 25000, 3000, 10000};
    if (c.fineMinor > 1000000000 - fines[static_cast<u32>(offense)]) return false;
    c.fineMinor += fines[static_cast<u32>(offense)]; ++c.convictions; c.lastEvidenceId = eventId;
    c.needs[planetary_detail::N(PlanetaryNeed::Safety)] = planetary_detail::Raise(c.needs[planetary_detail::N(PlanetaryNeed::Safety)], 250);
    return true;
}
void PlanetaryCity::Reprice(Firm& f) {
    for (u32 g = 0; g < kPlanetaryGoodCount; ++g) {
        const u32 level = std::max<u32>(1, f.stock[g]);
        const i64 ratio = std::clamp<i64>(static_cast<i64>(f.targetStock) * 1000 / level, 500, 2500);
        f.price[g] = std::max<i64>(1, config_.basePricesMinor[g] * ratio / 1000);
    }
}
bool PlanetaryCity::Purchase(u32 citizen, Firm& f, u32 good, u32 quantity) {
    auto& c = citizens_[citizen];
    if (quantity == 0 || quantity > 20 || good >= kPlanetaryGoodCount || f.stock[good] < quantity || c.inventory[good] > 65535u - quantity) return false;
    const i64 retail = f.price[good] * quantity;
    const i64 tax = Money(retail).ApplyRate(config_.salesTaxBasisPoints).Raw();
    if (Balance(citizen) < retail + tax) return false;
    if (!ledger_.Transfer(accounts_[citizen], f.account, Money(retail + tax))) return false;
    const bool moved = ledger_.Transfer(f.account, ledger_.CentralBank(), Money(tax)); LC_ASSERT(moved);
    f.stock[good] -= quantity; c.inventory[good] = static_cast<u16>(c.inventory[good] + quantity);
    retailSalesMinor_ += retail; taxesCollectedMinor_ += tax; Reprice(f);
    return true;
}
void PlanetaryCity::Consume(PlanetaryCitizen& c, u32 good) {
    using namespace planetary_detail;
    --c.inventory[good]; ++c.waste;
    if (good == G(PlanetaryGood::Food)) { c.needs[N(PlanetaryNeed::Hunger)] = Lower(c.needs[N(PlanetaryNeed::Hunger)], 700); c.needs[N(PlanetaryNeed::Energy)] = Lower(c.needs[N(PlanetaryNeed::Energy)], 200); }
    else if (good == G(PlanetaryGood::Water)) c.needs[N(PlanetaryNeed::Thirst)] = Lower(c.needs[N(PlanetaryNeed::Thirst)], 800);
    else if (good == G(PlanetaryGood::Medicine)) c.needs[N(PlanetaryNeed::Health)] = Lower(c.needs[N(PlanetaryNeed::Health)], 500);
    else if (good == G(PlanetaryGood::ConsumerGoods)) { c.needs[N(PlanetaryNeed::Hygiene)] = Lower(c.needs[N(PlanetaryNeed::Hygiene)], 500); c.needs[N(PlanetaryNeed::Comfort)] = Lower(c.needs[N(PlanetaryNeed::Comfort)], 500); }
    else if (good == G(PlanetaryGood::Electronics)) c.needs[N(PlanetaryNeed::Social)] = Lower(c.needs[N(PlanetaryNeed::Social)], 700);
}
PlanetaryResult PlanetaryCity::Apply(const PlanetaryCommand& command) {
    using namespace planetary_detail;
    if (command.citizen >= citizens_.size()) return PlanetaryResult::InvalidCitizen;
    auto& c = citizens_[command.citizen];
    if (!c.alive) return PlanetaryResult::Dead;
    if (command.sequence == 0 || command.sequence <= c.lastSequence) return PlanetaryResult::Duplicate;
    const u32 g = static_cast<u32>(command.good);
    Firm* f = FindFirm(command.building);
    const bool located = f && c.presence == command.building;
    switch (command.type) {
    case PlanetaryCommandType::Purchase:
        if (!f || f->building.kind != PlanetaryBuildingKind::Shop || g >= kPlanetaryGoodCount || command.quantity == 0 || command.quantity > 20 || c.inventory[g] > 65535u - command.quantity) return PlanetaryResult::InvalidCommand;
        if (!located) return PlanetaryResult::WrongLocation;
        if (f->stock[g] < command.quantity) return PlanetaryResult::OutOfStock;
        if (!Purchase(command.citizen, *f, g, command.quantity)) return PlanetaryResult::InsufficientFunds;
        break;
    case PlanetaryCommandType::Consume:
        if (g >= kPlanetaryGoodCount || (g != 0 && g != 1 && g != 5 && g != 6 && g != 7)) return PlanetaryResult::InvalidCommand;
        if (c.inventory[g] == 0) return PlanetaryResult::NoInventory;
        Consume(c, g); break;
    case PlanetaryCommandType::ApplyJob:
        if (!located) return PlanetaryResult::WrongLocation;
        if (f->building.kind == PlanetaryBuildingKind::Home) return PlanetaryResult::NoJob;
        c.work = command.building; c.workMinutes = 0; c.activity = PlanetaryActivity::Home; break;
    case PlanetaryCommandType::StartWork:
        if (c.work == kPlanetaryInvalidBuilding) return PlanetaryResult::NoJob;
        if (!located || c.work != command.building) return PlanetaryResult::WrongLocation;
        c.activity = PlanetaryActivity::Working; break;
    case PlanetaryCommandType::StopWork:
        c.activity = PlanetaryActivity::Home; break;
    case PlanetaryCommandType::Rest:
        if (c.presence != c.home) return PlanetaryResult::WrongLocation;
        c.activity = PlanetaryActivity::Resting; break;
    case PlanetaryCommandType::PayFine:
        if (!located || f->building.kind != PlanetaryBuildingKind::Civic) return PlanetaryResult::WrongLocation;
        if (!ledger_.Transfer(accounts_[command.citizen], ledger_.CentralBank(), Money(c.fineMinor))) return PlanetaryResult::InsufficientFunds;
        finesCollectedMinor_ += c.fineMinor; c.fineMinor = 0; break;
    case PlanetaryCommandType::RentHome:
        if (!located || f->building.kind != PlanetaryBuildingKind::Home) return PlanetaryResult::WrongLocation;
        if (!ledger_.Transfer(accounts_[command.citizen], f->account, Money(config_.dailyRentMinor))) return PlanetaryResult::InsufficientFunds;
        c.home = command.building; c.needs[N(PlanetaryNeed::Comfort)] = Lower(c.needs[N(PlanetaryNeed::Comfort)], 400); break;
    case PlanetaryCommandType::PayBills:
        if (c.presence != c.home) return PlanetaryResult::WrongLocation;
        if (!ledger_.Transfer(accounts_[command.citizen], ledger_.CentralBank(), Money(c.billsDueMinor))) return PlanetaryResult::InsufficientFunds;
        c.billsDueMinor = 0; c.utilitiesConnected = true; break;
    case PlanetaryCommandType::Wash:
        if (c.presence != c.home) return PlanetaryResult::WrongLocation;
        if (!c.utilitiesConnected) return PlanetaryResult::UtilitiesDisconnected;
        c.needs[N(PlanetaryNeed::Hygiene)] = 0; break;
    case PlanetaryCommandType::Recycle: {
        if (!located || (f->building.kind != PlanetaryBuildingKind::Civic && f->building.kind != PlanetaryBuildingKind::Shop)) return PlanetaryResult::WrongLocation;
        if (c.waste == 0) return PlanetaryResult::NoInventory;
        recycledUnits_ += c.waste;
        Firm* shop = f->building.kind == PlanetaryBuildingKind::Shop ? f : FindFirm(shops_.front());
        shop->stock[G(PlanetaryGood::RawMaterials)] = std::min<u32>(1000000, shop->stock[G(PlanetaryGood::RawMaterials)] + c.waste / 2);
        c.waste = 0; Reprice(*shop); break;
    }
    case PlanetaryCommandType::SellRawMaterials: {
        // The host escrow supplies mined units as validated command inputs, exactly as
        // factory production supplies new physical goods; the payment is never minted.
        if (!located || f->building.kind != PlanetaryBuildingKind::Shop) return PlanetaryResult::WrongLocation;
        if (command.quantity == 0 || command.quantity > 20) return PlanetaryResult::InvalidCommand;
        const i64 revenue = f->price[G(PlanetaryGood::RawMaterials)] * command.quantity / 2;
        if (!ledger_.Transfer(f->account, accounts_[command.citizen], Money(revenue))) return PlanetaryResult::InsufficientFunds;
        f->stock[G(PlanetaryGood::RawMaterials)] = std::min<u32>(1000000, f->stock[G(PlanetaryGood::RawMaterials)] + command.quantity); Reprice(*f); break;
    }
    default: return PlanetaryResult::InvalidCommand;
    }
    c.lastSequence = command.sequence; return PlanetaryResult::Accepted;
}

void PlanetaryCity::Step() {
    if (citizens_.empty()) return;
    const u32 slice = static_cast<u32>(tick_ % config_.ticksPerMinute);
    ++tick_;
    for (u32 i = slice; i < citizens_.size(); i += config_.ticksPerMinute) Minute(i);
    if (tick_ % config_.ticksPerMinute == 0) Market();
}
void PlanetaryCity::Minute(u32 index) {
    using namespace planetary_detail;
    auto& c = citizens_[index]; if (!c.alive) return;
    auto& n = c.needs;
    n[N(PlanetaryNeed::Hunger)] = Raise(n[N(PlanetaryNeed::Hunger)], 4);
    n[N(PlanetaryNeed::Thirst)] = Raise(n[N(PlanetaryNeed::Thirst)], 6);
    n[N(PlanetaryNeed::Hygiene)] = Raise(n[N(PlanetaryNeed::Hygiene)], 2);
    n[N(PlanetaryNeed::Rest)] = Raise(n[N(PlanetaryNeed::Rest)], 2);
    n[N(PlanetaryNeed::Energy)] = Raise(n[N(PlanetaryNeed::Energy)], 2);
    n[N(PlanetaryNeed::Social)] = Raise(n[N(PlanetaryNeed::Social)], 1);
    n[N(PlanetaryNeed::Safety)] = Lower(n[N(PlanetaryNeed::Safety)], 1);
    n[N(PlanetaryNeed::Comfort)] = c.utilitiesConnected ? Lower(n[N(PlanetaryNeed::Comfort)], 1) : Raise(n[N(PlanetaryNeed::Comfort)], 5);
    const u32 minute = (config_.startMinute + static_cast<u32>(((tick_ - 1) / config_.ticksPerMinute) % 1440)) % 1440;
    if (!c.controlled) {
        c.activity = PlanetaryActivity::Home; c.goalBuilding = c.home;
        if (minute >= c.shiftStartMinute && minute < c.shiftEndMinute && c.work != kPlanetaryInvalidBuilding) { c.activity = PlanetaryActivity::Working; c.goalBuilding = c.work; }
        else if (minute >= 22 * 60 || minute < 6 * 60) c.activity = PlanetaryActivity::Resting;
        else { c.activity = PlanetaryActivity::Socializing; n[N(PlanetaryNeed::Social)] = Lower(n[N(PlanetaryNeed::Social)], 10); }
        if (!c.embodied) c.presence = c.goalBuilding;
        for (u32 g = 0; g < 2; ++g) {
            const u32 need = g == 0 ? N(PlanetaryNeed::Hunger) : N(PlanetaryNeed::Thirst);
            if (n[need] >= 450) {
                if (c.inventory[g] == 0) {
                    for (u32 s = 0; s < shops_.size(); ++s) {
                        Firm* shop = FindFirm(shops_[(index + s) % shops_.size()]);
                        if (shop->stock[g] == 0 || Balance(index) < Price(shop->building.index, static_cast<PlanetaryGood>(g))) continue;
                        c.activity = PlanetaryActivity::Shopping; c.goalBuilding = shop->building.index;
                        if (!c.embodied) c.presence = c.goalBuilding;
                        if (c.presence == shop->building.index) Purchase(index, *shop, g, 1);
                        break;
                    }
                }
                if (c.inventory[g] != 0) Consume(c, g);
            }
        }
        if (c.presence == c.home && c.utilitiesConnected) n[N(PlanetaryNeed::Hygiene)] = Lower(n[N(PlanetaryNeed::Hygiene)], 15);
    }
    if (c.activity == PlanetaryActivity::Resting && c.presence == c.home) {
        n[N(PlanetaryNeed::Rest)] = Lower(n[N(PlanetaryNeed::Rest)], 14);
        n[N(PlanetaryNeed::Energy)] = Lower(n[N(PlanetaryNeed::Energy)], 12);
        n[N(PlanetaryNeed::Health)] = Lower(n[N(PlanetaryNeed::Health)], 2);
    }
    if (c.activity == PlanetaryActivity::Working && c.presence == c.work && c.work != kPlanetaryInvalidBuilding) {
        if (++c.workMinutes >= 60) {
            c.workMinutes = 0; Firm* employer = FindFirm(c.work);
            // Public service/output contracts circulate existing reserves through payroll.
            const i64 contract = config_.hourlyWageMinor * 4 / 3;
            ledger_.Transfer(ledger_.CentralBank(), employer->account, Money(contract));
            const i64 tax = Money(config_.hourlyWageMinor).ApplyRate(config_.incomeTaxBasisPoints).Raw();
            if (ledger_.BalanceOf(employer->account).Raw() >= config_.hourlyWageMinor) {
                ledger_.Transfer(employer->account, accounts_[index], Money(config_.hourlyWageMinor - tax));
                ledger_.Transfer(employer->account, ledger_.CentralBank(), Money(tax));
                wagesPaidMinor_ += config_.hourlyWageMinor; taxesCollectedMinor_ += tax;
            }
        }
    }
    if (minute == 0) {
        Firm* landlord = FindFirm(c.home);
        if (!ledger_.Transfer(accounts_[index], landlord->account, Money(config_.dailyRentMinor))) n[N(PlanetaryNeed::Comfort)] = Raise(n[N(PlanetaryNeed::Comfort)], 150);
        c.billsDueMinor = std::min<i64>(1000000000, c.billsDueMinor + config_.dailyUtilitiesMinor);
        if (!c.controlled && ledger_.Transfer(accounts_[index], ledger_.CentralBank(), Money(c.billsDueMinor))) c.billsDueMinor = 0;
        c.utilitiesConnected = c.billsDueMinor <= config_.dailyUtilitiesMinor;
        if (!c.controlled && c.waste != 0) {
            recycledUnits_ += c.waste / 2; Firm* shop = FindFirm(shops_[index % shops_.size()]);
            shop->stock[G(PlanetaryGood::RawMaterials)] = std::min<u32>(1000000, shop->stock[G(PlanetaryGood::RawMaterials)] + c.waste / 4); c.waste = 0;
        }
    }
    if (n[N(PlanetaryNeed::Hunger)] >= 980 || n[N(PlanetaryNeed::Thirst)] >= 980) Damage(index, 2);
}
void PlanetaryCity::Market() {
    for (auto& f : firms_) {
        if (f.building.kind != PlanetaryBuildingKind::Shop) continue;
        for (u32 g = 0; g < kPlanetaryGoodCount; ++g) {
            if (f.stock[g] >= f.targetStock) continue;
            const u32 quantity = std::min<u32>(8, f.targetStock - f.stock[g]);
            const i64 wholesale = std::max<i64>(1, config_.basePricesMinor[g] / 2) * quantity;
            // Wholesalers produce against funded orders. Advanced goods additionally use
            // material inputs from this stock; goods are physical units, never currency.
            const u32 input = (g == 4 || g == 6) ? 3u : (g == 5 || g == 7) ? 4u : kPlanetaryGoodCount;
            if (input < kPlanetaryGoodCount && f.stock[input] < quantity) continue;
            if (ledger_.Transfer(f.account, ledger_.CentralBank(), Money(wholesale))) {
                if (input < kPlanetaryGoodCount) f.stock[input] -= quantity;
                f.stock[g] += quantity;
            }
        }
        Reprice(f);
    }
}
PlanetaryCityStats PlanetaryCity::Stats() const {
    PlanetaryCityStats s;
    s.population = CitizenCount(); s.minuteOfDay = MinuteOfDay(); s.tick = tick_;
    s.totalMoneyMinor = ledger_.SumOfBalances().Raw(); s.issuedMoneyMinor = ledger_.TotalIssued().Raw();
    if (ledger_.HasCentralBank()) s.governmentMinor = ledger_.BalanceOf(ledger_.CentralBank()).Raw();
    s.wagesPaidMinor = wagesPaidMinor_; s.retailSalesMinor = retailSalesMinor_; s.taxesCollectedMinor = taxesCollectedMinor_; s.finesCollectedMinor = finesCollectedMinor_; s.recycledUnits = recycledUnits_;
    for (const auto& c : citizens_) {
        if (!c.alive) continue;
        ++s.living; if (c.work == kPlanetaryInvalidBuilding) ++s.unemployed;
        if (c.needs[0] >= 550) ++s.hungry;
        if (!c.utilitiesConnected) ++s.disconnected;
        s.waste += c.waste;
    }
    return s;
}
void PlanetaryCity::HashInto(Hasher& h) const {
    h.U64(seed_); h.U64(tick_); planetary_detail::HashConfig(h, config_); ledger_.HashInto(h);
    for (const auto& c : citizens_) planetary_detail::HashCitizen(h, c);
    for (const auto& f : firms_) {
        h.U32(f.building.index); h.U8(static_cast<u8>(f.building.kind));
        for (u32 s : f.stock) h.U32(s);
        for (i64 p : f.price) h.I64(p);
    }
    h.I64(wagesPaidMinor_); h.I64(retailSalesMinor_); h.I64(taxesCollectedMinor_); h.I64(finesCollectedMinor_); h.U32(recycledUnits_);
}
u64 PlanetaryCity::Hash() const { Hasher h; HashInto(h); return h.Value(); }

std::vector<u8> PlanetaryCity::SaveDelta() const {
    using namespace planetary_detail;
    Writer w;
    w.bytes.reserve(citizens_.size() * 128 + firms_.size() * 128 + 256);
    w.U(0x5043495459444c54ull); w.U(2, 4); w.U(seed_); w.U(tick_); w.U(static_cast<u64>(ledger_.TotalIssued().Raw()));
    Hasher layout; HashConfig(layout, config_);
    for (const auto& f : firms_) { layout.U32(f.building.index); layout.U8(static_cast<u8>(f.building.kind)); }
    w.U(layout.Value()); w.U(Hash());
    w.U(static_cast<u64>(wagesPaidMinor_)); w.U(static_cast<u64>(retailSalesMinor_)); w.U(static_cast<u64>(taxesCollectedMinor_)); w.U(static_cast<u64>(finesCollectedMinor_)); w.U(recycledUnits_, 4);
    u32 changed = 0; for (u32 i = 0; i < citizens_.size(); ++i) if (!SameCitizen(citizens_[i], InitialCitizen(i))) ++changed;
    w.U(changed, 4);
    for (u32 i = 0; i < citizens_.size(); ++i) if (!SameCitizen(citizens_[i], InitialCitizen(i))) { w.U(i, 4); w.Citizen(citizens_[i]); }
    changed = 0;
    for (const auto& f : firms_) {
        const u32 initial = f.building.kind == PlanetaryBuildingKind::Shop ? f.targetStock : 0;
        if (std::any_of(f.stock.begin(), f.stock.end(), [initial](u32 v) { return v != initial; })) ++changed;
    }
    w.U(changed, 4);
    for (const auto& f : firms_) {
        const u32 initial = f.building.kind == PlanetaryBuildingKind::Shop ? f.targetStock : 0;
        if (std::any_of(f.stock.begin(), f.stock.end(), [initial](u32 v) { return v != initial; })) {
            w.U(f.building.index, 4); for (u32 s : f.stock) w.U(s, 4);
        }
    }
    changed = 0;
    auto initialBalance = [this](u32 index) { return index == 0 ? config_.governmentReserveMinor : index <= firms_.size() ? config_.initialFirmCapitalMinor : config_.initialCitizenCashMinor; };
    for (u32 i = 0; i < ledger_.AccountCount(); ++i) if (ledger_.BalanceOf(ledger_.AccountAt(i)).Raw() != initialBalance(i)) ++changed;
    w.U(changed, 4);
    for (u32 i = 0; i < ledger_.AccountCount(); ++i) {
        const i64 delta = ledger_.BalanceOf(ledger_.AccountAt(i)).Raw() - initialBalance(i);
        if (delta != 0) { w.U(i, 4); w.U(static_cast<u64>(delta)); }
    }
    Hasher checksum; checksum.Bytes(w.bytes.data(), w.bytes.size()); w.U(checksum.Value());
    return std::move(w.bytes);
}
bool PlanetaryCity::LoadDelta(std::span<const u8> bytes) {
    using namespace planetary_detail;
    if (citizens_.empty() || bytes.size() < 100 || bytes.size() > citizens_.size() * 160 + firms_.size() * 80 + 512) return false;
    Hasher checksum; checksum.Bytes(bytes.data(), bytes.size() - 8);
    Reader tail{bytes.subspan(bytes.size() - 8)}; if (tail.U() != checksum.Value()) return false;
    Reader r{bytes.first(bytes.size() - 8)};
    if (r.U() != 0x5043495459444c54ull || r.U(4) != 2 || r.U() != seed_) return false;
    const u64 tick = r.U();
    const i64 issued = static_cast<i64>(r.U()); if (issued < 0) return false;
    Hasher layout; HashConfig(layout, config_);
    for (const auto& f : firms_) { layout.U32(f.building.index); layout.U8(static_cast<u8>(f.building.kind)); }
    if (r.U() != layout.Value()) return false;
    const u64 expectedHash = r.U();
    PlanetaryCity next;
    std::vector<PlanetaryBuilding> buildings; buildings.reserve(firms_.size());
    for (const auto& f : firms_) buildings.push_back(f.building);
    if (!next.Generate(seed_, buildings, config_)) return false;
    next.tick_ = tick;
    next.wagesPaidMinor_ = static_cast<i64>(r.U()); next.retailSalesMinor_ = static_cast<i64>(r.U()); next.taxesCollectedMinor_ = static_cast<i64>(r.U()); next.finesCollectedMinor_ = static_cast<i64>(r.U()); next.recycledUnits_ = static_cast<u32>(r.U(4));
    if (next.wagesPaidMinor_ < 0 || next.retailSalesMinor_ < 0 || next.taxesCollectedMinor_ < 0 || next.finesCollectedMinor_ < 0) return false;
    u32 count = static_cast<u32>(r.U(4)); if (count > CitizenCount()) return false;
    u32 prior = 0;
    for (u32 i = 0; i < count; ++i) {
        const u32 ordinal = static_cast<u32>(r.U(4));
        if (ordinal >= CitizenCount() || (i > 0 && ordinal <= prior)) return false;
        prior = ordinal; auto& c = next.citizens_[ordinal]; r.Citizen(c);
        const Firm* home = next.FindFirm(c.home); const Firm* work = next.FindFirm(c.work);
        if (!home || home->building.kind != PlanetaryBuildingKind::Home || (c.work != kPlanetaryInvalidBuilding && (!work || work->building.kind == PlanetaryBuildingKind::Home))) return false;
        if (c.presence != kPlanetaryInvalidBuilding && !next.FindFirm(c.presence)) return false;
        if (c.identity == 0 || (c.goalBuilding != kPlanetaryInvalidBuilding && !next.FindFirm(c.goalBuilding))) return false;
        if (c.shiftStartMinute >= c.shiftEndMinute || c.shiftEndMinute >= 1440) return false;
        if (static_cast<u8>(c.activity) > static_cast<u8>(PlanetaryActivity::Dead) || c.fineMinor < 0 || c.fineMinor > 1000000000 || c.billsDueMinor < 0 || c.billsDueMinor > 1000000000 || c.workMinutes >= 60) return false;
        for (u16 n : c.needs) if (n > 1000) return false;
        if ((!c.alive && (c.activity != PlanetaryActivity::Dead || c.needs[N(PlanetaryNeed::Health)] != 1000)) || (c.alive && (c.activity == PlanetaryActivity::Dead || c.needs[N(PlanetaryNeed::Health)] == 1000))) return false;
    }
    count = static_cast<u32>(r.U(4)); if (count > firms_.size()) return false;
    prior = 0;
    for (u32 i = 0; i < count; ++i) {
        const u32 building = static_cast<u32>(r.U(4)); Firm* f = next.FindFirm(building);
        if (!f || (i > 0 && building <= prior)) return false;
        prior = building;
        for (u32& stock : f->stock) { stock = static_cast<u32>(r.U(4)); if (stock > 1000000) return false; }
        next.Reprice(*f);
    }
    std::vector<i64> balances; balances.reserve(next.ledger_.AccountCount());
    for (u32 i = 0; i < next.ledger_.AccountCount(); ++i) balances.push_back(next.ledger_.BalanceOf(next.ledger_.AccountAt(i)).Raw());
    count = static_cast<u32>(r.U(4)); if (count > balances.size()) return false;
    prior = 0;
    for (u32 i = 0; i < count; ++i) {
        const u32 account = static_cast<u32>(r.U(4)); const i64 delta = static_cast<i64>(r.U());
        if (account >= balances.size() || (i > 0 && account <= prior)) return false;
        prior = account;
        if (delta < -balances[account] || delta > std::numeric_limits<i64>::max() - balances[account]) return false;
        balances[account] += delta;
    }
    if (!r.good || r.pos != r.bytes.size()) return false;
    i64 sum = 0;
    for (i64 b : balances) { if (b < 0 || b > issued - sum) return false; sum += b; }
    if (sum != issued) return false;
    Ledger restored(static_cast<u32>(balances.size()));
    const AccountId bank = restored.OpenAccount(); restored.SetCentralBank(bank); restored.Mint(Money(issued));
    for (u32 i = 1; i < balances.size(); ++i) { const AccountId id = restored.OpenAccount(); if (!restored.Transfer(bank, id, Money(balances[i]))) return false; }
    next.ledger_ = std::move(restored);
    if (next.Hash() != expectedHash || !next.ConservationHolds()) return false;
    *this = std::move(next); return true;
}
} // namespace lc
