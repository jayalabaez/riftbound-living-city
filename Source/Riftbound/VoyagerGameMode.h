#pragma once
#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/SaveGame.h"
#include "Async/Future.h"
#include "VoyagerCityLife.h"
#include "VoyagerItems.h"
#include "VoyagerDestruction.h"
#include "VoyagerGameMode.generated.h"
class AVoyagerCharacter;
class AVoyagerShip;
class AVoyagerWorld;

UCLASS()
class RIFTBOUND_API AVoyagerState : public AGameStateBase
{
    GENERATED_BODY()
public:
    AVoyagerState();
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    UPROPERTY(Replicated) int32 SystemSeed=1;
    UPROPERTY(Replicated) int32 PlanetIndex=0;
    UPROPERTY(Replicated) int32 Mode=0;
    UPROPERTY(Replicated) int32 Revision=0;
    UPROPERTY(Replicated) bool bTransitioning=false;
    UPROPERTY(Replicated) float TransitionTime=0;
    UPROPERTY(Replicated) FString TransitionLabel;
};

UCLASS()
class RIFTBOUND_API AVoyagerPlayerState : public APlayerState
{
    GENERATED_BODY()
public:
    UPROPERTY(Replicated) TArray<int32> Items=VoyagerItems::StartingInventory();
    int32 ItemCount(EVoyagerItem Item) const;
    bool AddItem(EVoyagerItem Item,int32 Quantity);
    bool TakeItem(EVoyagerItem Item,int32 Quantity);
    bool GrantHuntLoot(int32 Meat,int32 Hide,int32 Bone);
    UPROPERTY(Replicated) int32 Minerals=0;
    UPROPERTY(Replicated) int32 Discoveries=0;
    UPROPERTY(Replicated) int32 PirateKills=0;
    UPROPERTY(Replicated) int32 Upgrades=0;
    UPROPERTY(Replicated) int32 WantedStars=0;
    UPROPERTY(Replicated) bool bLawSearching=false;
    UPROPERTY(Replicated) float WantedSearchSeconds=0;
    float CrimeHeat=0,LastLawContact=0,NextLawDispatch=0;
    FVector LastKnownPosition=FVector::ZeroVector;
    UPROPERTY() TArray<int64> Visited;
    void AddMinerals(int32 Amount);
    bool DiscoverPlanet(int32 System,int32 Planet);
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
};

UCLASS()
class RIFTBOUND_API UVoyagerSave : public USaveGame
{
    GENERATED_BODY()
public:
    UPROPERTY() int32 Version=5;
    UPROPERTY() TArray<int32> Items=VoyagerItems::StartingInventory();
    UPROPERTY() TArray<FVoyagerBuildingDamage> BuildingDamage;
    UPROPERTY() float JailSeconds=0;
    UPROPERTY() int32 JailSystem=0;
    UPROPERTY() int32 JailPlanet=0;
    UPROPERTY() int32 JailSite=0;
    UPROPERTY() TArray<FVoyagerCityArchive> CityArchives;
    UPROPERTY() TArray<int32> CityResidentLocations;
    UPROPERTY() int32 SystemSeed=1;
    UPROPERTY() int32 PlanetIndex=0;
    UPROPERTY() int32 Minerals=0;
    UPROPERTY() int32 PirateKills=0;
    UPROPERTY() int32 Upgrades=0;
    UPROPERTY() TArray<int64> Visited;
};

UCLASS()
class RIFTBOUND_API AVoyagerGameMode : public AGameModeBase
{
    GENERATED_BODY()
public:
    AVoyagerGameMode();
    virtual void InitGame(const FString& MapName,const FString& Options,FString& ErrorMessage) override;
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void PostLogin(APlayerController* NewPlayer) override;
    virtual void Logout(AController* Exiting) override;
    virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;
    virtual void PreLogin(const FString& Options,const FString& Address,const FUniqueNetIdRepl& UniqueId,FString& ErrorMessage) override;
    void BoardShip(AVoyagerCharacter* Explorer);
    void LeaveShip(AVoyagerShip* Ship);
    void EnterSpace(AController* Pilot);
    void LandOnPlanet(AController* Pilot,int32 Planet);
    void WarpToPlanet(AController* Pilot,int32 Planet);
    void NextSystem(AController* Pilot);
    void RecoverShip(AVoyagerShip* Ship);
    void RecoverExplorer(AVoyagerCharacter* Explorer);
    bool SaveExpedition(bool bImmediate=false,AVoyagerCityLife* SaveSource=nullptr);
    void UpgradeShip(AController* Pilot);
    AVoyagerShip* ShipFor(AController* Pilot);
private:
    UPROPERTY() TObjectPtr<AVoyagerWorld> WorldBuilder;
    UPROPERTY() TObjectPtr<UVoyagerSave> LoadedSave;
    UPROPERTY() TObjectPtr<UVoyagerSave> PendingSave;
    UPROPERTY() TMap<TObjectPtr<AController>,TObjectPtr<AVoyagerShip>> Ships;
    UPROPERTY() TArray<TObjectPtr<AActor>> Starts;
    int32 StartIndex=0;
    int32 PendingMode=0,PendingSystem=1,PendingPlanet=0;
    bool bPendingWarp=false;
    bool bLoadedHost=false;
    float PirateTimer=2.f;
    TFuture<TArray<FVoyagerCityArchive>> SaveEncoding;
    TFuture<bool> SaveWriting;
    bool bSaveRequested=false,bEndingPlay=false,bSaveSystemPrimed=false;
    double NextAutosaveTime=0;
    uint64 NextSaveSerial=0,ActiveSaveSerial=0,LastWrittenSaveSerial=0;
    FString ActiveSaveSlot;
    // Map travel destroys the local controller before city EndPlay. Keep only the
    // last settled expedition fields so that final city snapshots retain its cargo.
    bool bHaveCapturedHost=false;
    int32 CapturedMinerals=0,CapturedPirateKills=0,CapturedUpgrades=0;
    TArray<int64> CapturedVisited;
    TArray<int32> CapturedItems=VoyagerItems::StartingInventory();
    TArray<FVoyagerBuildingDamage> CapturedBuildingDamage;
    float CapturedJailSeconds=0;
    int32 CapturedJailSystem=0,CapturedJailPlanet=0,CapturedJailSite=0;
    bool bPendingCustodyRestore=false;
    TWeakObjectPtr<APlayerController> CustodyRestoreController;
    bool StartSave(bool bWaitForCommands,AVoyagerCityLife* SaveSource=nullptr);
    bool PumpSave(bool bWait);
    void BeginTravel(int32 Mode,int32 System,int32 Planet,const FString& Label,float Duration,bool WarpOnly=false);
    void FinishTravel();
    void SpawnPirates();
    void ClearPirates();
};
