#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "RiftGameMode.generated.h"

class APlayerStart;

UCLASS()
class RIFTBOUND_API ARiftGameState : public AGameStateBase
{
    GENERATED_BODY()
public:
    ARiftGameState();
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    // 0 exploration, 1 rupture, 2 active wave, 3 intermission, 4 victory, 5 defeat.
    UPROPERTY(Replicated, BlueprintReadOnly) int32 Phase = 0;
    UPROPERTY(Replicated, BlueprintReadOnly) int32 Wave = 0;
    UPROPERTY(Replicated, BlueprintReadOnly) int32 Remaining = 0;
    UPROPERTY(Replicated, BlueprintReadOnly) int32 TeamKills = 0;
    UPROPERTY(Replicated, BlueprintReadOnly) int32 Anomaly = 0;
    UPROPERTY(Replicated, BlueprintReadOnly) float PhaseTime = 0.f;
    UPROPERTY(Replicated, BlueprintReadOnly) FString Objective;
};

UCLASS()
class RIFTBOUND_API ARiftGameMode : public AGameModeBase
{
    GENERATED_BODY()
public:
    ARiftGameMode();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void PreLogin(const FString& Options, const FString& Address,
        const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage) override;
    virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;

    UFUNCTION(BlueprintCallable) void TriggerAnomaly();
    void EnemyKilled(AController* Killer);
    UFUNCTION(BlueprintCallable) void RestartRun();

private:
    void StartWave();
    void CompleteWave();
    void RefillTeam();
    void ClearThreats();
    int32 ActivePlayerCount() const;
    UPROPERTY() TArray<TObjectPtr<APlayerStart>> SpawnPoints;
    int32 SpawnCursor = 0;
    float AllDownTime = 0.f;
    float RecountTime = 0.f;
};
