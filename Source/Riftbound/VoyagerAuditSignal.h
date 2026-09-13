#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VoyagerAuditSignal.generated.h"

/** Spawned only by opt-in integration audits; acknowledges actual peer observations. */
UCLASS()
class RIFTBOUND_API AVoyagerAuditSignal : public AActor
{
    GENERATED_BODY()
public:
    AVoyagerAuditSignal();
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
    UPROPERTY(Replicated) uint8 Kind=0; // 1 hunting; 2 destruction.
    UPROPERTY(Replicated) TObjectPtr<AActor> Subject;
    UPROPERTY(Replicated) uint8 Acknowledged=0;
    UFUNCTION(Server,Reliable) void ServerAcknowledge(uint8 Phase);
    static AVoyagerAuditSignal* Find(UWorld* World,APlayerController* Owner,uint8 Kind);
    static AVoyagerAuditSignal* EnsurePeer(UWorld* World,uint8 Kind);
};
