#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoyagerCityLife.generated.h"

class UVoyagerSave;
class AVoyagerPlayerState;
class FVoyagerCityLifeHost;
struct FVoyagerCityLifeHostDeleter { void operator()(FVoyagerCityLifeHost* Host) const; };

UENUM()
enum class EVoyagerCityAction : uint8
{
    BuyGood, ConsumeGood, ApplyJob, StartWork, StopWork, Rest, Wash,
    PayBills, RentHome, PayFine, SellMinerals, Recycle
};

USTRUCT()
struct FVoyagerCityGoodView
{
    GENERATED_BODY()
    UPROPERTY() FString Name;
    UPROPERTY() int64 PriceMinor=0;
    UPROPERTY() int32 Stock=0;
    UPROPERTY() int32 Owned=0;
};

USTRUCT()
struct FVoyagerCitizenLifeView
{
    GENERATED_BODY()
    UPROPERTY() bool bReady=false;
    UPROPERTY() bool bAlive=true;
    UPROPERTY() int32 Home=0;
    UPROPERTY() int32 Workplace=0;
    UPROPERTY() int32 GoalBuilding=INDEX_NONE;
    UPROPERTY() int32 Activity=0;
    UPROPERTY() int32 Health=100;
    UPROPERTY() int32 ShiftStart=8;
    UPROPERTY() int32 ShiftEnd=16;
    UPROPERTY() int64 MoneyMinor=0;
    UPROPERTY() int64 FineMinor=0;
    UPROPERTY() TArray<int32> Needs;
};

USTRUCT()
struct FVoyagerCityLifeView
{
    GENERATED_BODY()
    UPROPERTY() bool bAvailable=false;
    UPROPERTY() bool bAtCity=false;
    UPROPERTY() bool bOnFoot=false;
    UPROPERTY() bool bEmployed=false;
    UPROPERTY() bool bRented=false;
    UPROPERTY() bool bUtilitiesConnected=true;
    UPROPERTY() bool bWorking=false;
    UPROPERTY() int32 CityKey=0;
    UPROPERTY() int32 ResidentId=0;
    UPROPERTY() int32 Population=0;
    UPROPERTY() int32 SystemPopulation=0;
    UPROPERTY() int32 CurrentBuilding=INDEX_NONE;
    UPROPERTY() int32 HomeBuilding=0;
    UPROPERTY() int32 WorkplaceBuilding=0;
    UPROPERTY() int32 CriminalRecord=0;
    UPROPERTY() int64 Tick=0;
    // Worker input acknowledgement captured atomically with this snapshot's needs.
    UPROPERTY() uint64 ProcessedInput=0;
    UPROPERTY() int64 CreditsMinor=0;
    UPROPERTY() int64 WageMinor=0;
    UPROPERTY() int64 RentMinor=0;
    UPROPERTY() int64 BillsMinor=0;
    UPROPERTY() int64 FineMinor=0;
    UPROPERTY() int64 GovernmentMinor=0;
    UPROPERTY() int32 Waste=0;
    UPROPERTY() float TickMilliseconds=0;
    UPROPERTY() FString CityName;
    UPROPERTY() FString ClockText;
    UPROPERTY() FString HomeName;
    UPROPERTY() FString WorkName;
    UPROPERTY() FString CurrentBuildingName;
    UPROPERTY() FString ShiftStatus;
    UPROPERTY() FString LegalStatus;
    // Pressure 0=satisfied, 1000=critical; health uses the same pressure convention.
    UPROPERTY() TArray<int32> Needs;
    UPROPERTY() TArray<FVoyagerCityGoodView> Goods;
    UPROPERTY() TArray<FString> Bulletin;
};

USTRUCT()
struct FVoyagerCityArchive
{
    GENERATED_BODY()
    UPROPERTY() int32 System=1;
    UPROPERTY() int32 Planet=0;
    UPROPERTY() int32 Site=0;
    // Older saves use State. New saves use bounded Base64(zlib delta) in a string,
    // whose bulk serialization avoids reflected per-byte array serialization stalls.
    UPROPERTY() TArray<uint8> State;
    UPROPERTY() FString PackedState;
};

/** Unreal boundary: commands in, snapshots out; a worker owns the pure C++ city state. */
UCLASS()
class RIFTBOUND_API AVoyagerCityLife : public AActor
{
    GENERATED_BODY()
public:
    AVoyagerCityLife();
    virtual ~AVoyagerCityLife() override;
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
    static AVoyagerCityLife* Find(const UWorld* World);
    bool GetCitizen(int32 System,int32 Planet,int32 Site,int32 Ordinal,FVoyagerCitizenLifeView& Out) const;
    bool IsCitizenAlive(int32 System,int32 Planet,int32 Site,int32 Ordinal) const;
    void SetEmbodied(int32 System,int32 Planet,int32 Site,int32 Ordinal,bool bEmbodied);
    void ReportPresence(int32 System,int32 Planet,int32 Site,int32 Ordinal,int32 Building);
    void ApplyCitizenDamage(int32 System,int32 Planet,int32 Site,int32 Ordinal,int32 HealthRemaining);
    float CityHour() const { return ReplicatedHour; }
    void HandleAction(AController* Controller,uint8 Action,int32 Argument);
    FVoyagerCityLifeView BuildView(AController* Controller) const;
    void RecordCrime(AController* Offender,int32 Severity,int32 Confidence);
    void ApplyPlayerDamage(AController* Controller,float DamageApplied,float HealthRemaining);
    void RecoverPlayer(AController* Controller);
    bool ApplyFieldCare(AController* Controller,int32 Food,int32 Water,int32 Health);
    bool AuditConservation(int64& Total,int64& Issued) const;
    void SaveTo(UVoyagerSave* Save);
    // Captures matching city/cargo state on the game thread; the returned encoder owns
    // independent plain data and is safe to run after this actor/world is destroyed.
    bool TryPrepareSave(UVoyagerSave* Save,TUniqueFunction<TArray<FVoyagerCityArchive>()>& Encode,bool bWaitForCommands=false);
    static bool ValidateArchive(const FVoyagerCityArchive& Archive,int32 Ordinal,FVoyagerCitizenLifeView& OutResident,int64& TotalMinor,int64& IssuedMinor);
    void RestoreFrom(const UVoyagerSave* Save);
    bool IsReady() const;
private:
    TUniquePtr<FVoyagerCityLifeHost,FVoyagerCityLifeHostDeleter> Host;
    UPROPERTY() TArray<FVoyagerCityArchive> Archives;
    UPROPERTY(Replicated) float ReplicatedHour=9;
    UPROPERTY(Replicated) int32 ReplicatedPopulation=0;
    TMap<TWeakObjectPtr<AController>,int32> PlayerSlots;
    TMap<TWeakObjectPtr<AController>,double> LastCommandTimes;
    TMap<TWeakObjectPtr<AController>,float> LastCoreHealth;
    TMap<TWeakObjectPtr<AController>,TWeakObjectPtr<APawn>> LastCorePawn;
    // Authority projection after accepted damage or care; stale snapshots cannot
    // replace it before acknowledging the latest command that produced it.
    struct FPendingPlayerHealth { float Health=100;uint64 CommandId=0; };
    TMap<TWeakObjectPtr<AController>,FPendingPlayerHealth> PendingPlayerHealth;
    TMap<TWeakObjectPtr<AController>,uint64> PendingRecovery;
    TArray<int32> ResidentLocations;
    struct FPendingCityAction
    {
        TWeakObjectPtr<AController> Controller;
        EVoyagerCityAction Action=EVoyagerCityAction::BuyGood;
        int32 Minerals=0;
        TWeakObjectPtr<AVoyagerPlayerState> PlayerState;
    };
    TMap<uint64,FPendingCityAction> PendingActions;
    int32 ActiveSystem=INDEX_NONE;
    float SnapshotRemaining=0;
    void RefreshSystem(int32 System);
    bool SettleActions();
    bool SettleAction(uint64 Id,uint8 Result);
    int32 PlayerOrdinal(AController* Controller) const;
    int32 PlayerCityKey(AController* Controller) const;
};
