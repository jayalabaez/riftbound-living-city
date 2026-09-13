#pragma once

#include "CoreMinimal.h"
#include "Subsystems/LocalPlayerSubsystem.h"
#include "VoyagerLocalAI.generated.h"

class AVoyagerCitizen;
class AVoyagerController;
class IHttpRequest;

/** Optional presentation-only citizen dialogue; never runs on a dedicated server. */
UCLASS()
class RIFTBOUND_API UVoyagerLocalAI : public ULocalPlayerSubsystem
{
    GENERATED_BODY()
public:
    // Call AFTER displaying the server-authored reply. A failed or busy request leaves it intact.
    static void EnrichConversation(AVoyagerController* Controller, AVoyagerCitizen* Citizen, const FString& AuthoredReply);
    static void CancelConversation(AVoyagerController* Controller);
    virtual void Deinitialize() override;
    virtual void PlayerControllerChanged(APlayerController* NewPlayerController) override;

    // Shared with the development automation tests. Only validated display text crosses this boundary.
    static bool ValidateResponsePayload(const FString& Payload, FString& OutLine);
    static bool PreservesAuthoredFacts(const FString& AuthoredReply, const FString& Line);

private:
    void RequestLine(AVoyagerController* Controller, AVoyagerCitizen* Citizen, const FString& AuthoredReply);
    void FinishRequest(const FString& Payload, bool bTransportSucceeded, uint64 Serial);
    void CancelPending();
    bool CanApplyPending() const;
    TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> ActiveRequest;
    TWeakObjectPtr<AVoyagerController> PendingController;
    TWeakObjectPtr<AVoyagerCitizen> PendingCitizen;
    FString PendingAuthoredReply;
    FString PendingCacheKey;
    TMap<FString, FString> ReplyCache;
    TArray<FString> CacheOrder;
    uint64 RequestSerial = 0;
    double RequestStartedAt = 0.0;
};
