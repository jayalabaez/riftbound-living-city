#include "VoyagerLocalAI.h"
#include "VoyagerCharacter.h"
#include "VoyagerCitizen.h"
#include "VoyagerData.h"
#include "VoyagerGameMode.h"
#include "Dom/JsonObject.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/CommandLine.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
    // There is deliberately no editable remote endpoint or model download/pull path.
    constexpr const TCHAR* OllamaEndpoint = TEXT("http://127.0.0.1:11434/api/generate");
    constexpr const TCHAR* OllamaModel = TEXT("llama3.2:3b");
    constexpr float RequestTimeoutSeconds = 15.f;
    constexpr int32 MaximumReplyCharacters = 280;
    constexpr int32 MaximumCachedReplies = 32;
    TAutoConsoleVariable<int32> CVarLocalDialogue(TEXT("voyager.LocalAI"), 1,
        TEXT("Optional local Ollama citizen dialogue. 0 keeps all authored dialogue; 1 enables CPU-only requests."));

    // This budget is process-wide, including split-screen and multiple local worlds.
    TWeakObjectPtr<UVoyagerLocalAI> InFlightOwner;
    double NextRequestAllowedAt = 0.0;

    bool DialogueEnabled()
    {
        const TCHAR* CommandLine = FCommandLine::Get();
        if (CVarLocalDialogue.GetValueOnGameThread() == 0 || FParse::Param(CommandLine, TEXT("NoVoyagerLocalAI"))) return false;
        if (!FParse::Param(CommandLine, TEXT("VoyagerAITest")))
        {
            const TCHAR* StableDialogueTests[] = {TEXT("VoyagerTest"), TEXT("VoyagerNetTest"), TEXT("VoyagerSurfaceAudit"),
                TEXT("VoyagerLifeAudit"), TEXT("VoyagerCityAudit"), TEXT("VoyagerCityNetAudit"),
                TEXT("LivingCityIsolationAudit"), TEXT("VoyagerRealismAudit"), TEXT("VoyagerCrimeAudit"), TEXT("VoyagerBuildingAudit")};
            for (const TCHAR* Flag : StableDialogueTests) if (FParse::Param(CommandLine, Flag)) return false;
        }
        return true;
    }

    bool IsEligible(const AVoyagerController* Controller, const AVoyagerCitizen* Citizen)
    {
        if (!IsValid(Controller) || !IsValid(Citizen) || !Controller->IsLocalController() ||
            !Controller->GetLocalPlayer() || Controller->GetNetMode() == NM_DedicatedServer ||
            Controller->GetWorld() != Citizen->GetWorld() || Citizen->IsAlarmed() || !Citizen->IsAlive()) return false;
        const auto* State = Controller->GetWorld()->GetGameState<AVoyagerState>();
        return State && !State->bTransitioning && State->SystemSeed == Citizen->SystemIndex();
    }

    FString BuildRequest(const AVoyagerCitizen* Citizen, const FString& AuthoredReply)
    {
        const FString Facts = FString::Printf(
            TEXT("Citizen name: %s. Occupation: %s. Planet name: %s. Planet biome (not district name): %s. ")
            TEXT("District number: %d. Verified dialogue facts: %s\n")
            TEXT("Rewrite ONLY the verified dialogue facts, keeping every fact and schedule unchanged. ")
            TEXT("Use 1-2 sentences. Names and biography must come only from those facts. ")
            TEXT("Planet and biome are background context, not extra locations or claims to add."),
            *Citizen->DisplayName(), *Citizen->RoleName(),
            *Voyager::PlanetName(Citizen->SystemIndex(), Citizen->PlanetIndex()),
            Voyager::BiomeName(Voyager::Biome(Citizen->SystemIndex(), Citizen->PlanetIndex())),
            Citizen->SiteIndex() + 1, *AuthoredReply);

        auto Body = MakeShared<FJsonObject>();
        Body->SetStringField(TEXT("model"), OllamaModel);
        Body->SetStringField(TEXT("system"),
            TEXT("You are a game dialogue copy editor. Rephrase supplied dialogue without adding or changing facts. ")
            TEXT("Speak in first person, 1-2 short sentences, at most 40 words. Copy names and schedule words exactly. ")
            TEXT("Do not invent places, services, quests, dangers, controls, rewards or history. ")
            TEXT("Do not give the citizen a different job or home. No role labels, stage directions, markdown, code or URLs. ")
            TEXT("Return a JSON object with only a line string."));
        Body->SetStringField(TEXT("prompt"), Facts);
        Body->SetBoolField(TEXT("stream"), false);
        Body->SetStringField(TEXT("keep_alive"), TEXT("30s"));
        auto Options = MakeShared<FJsonObject>();
        Options->SetNumberField(TEXT("num_gpu"), 0);
        Options->SetNumberField(TEXT("num_ctx"), 1024);
        Options->SetNumberField(TEXT("num_predict"), 80);
        Options->SetNumberField(TEXT("num_thread"), 2);
        Options->SetNumberField(TEXT("temperature"), .3);
        Options->SetNumberField(TEXT("seed"), Citizen->SystemIndex() * 31 + Citizen->OrdinalIndex());
        Body->SetObjectField(TEXT("options"), Options);
        auto LineSchema = MakeShared<FJsonObject>();
        LineSchema->SetStringField(TEXT("type"), TEXT("string"));
        auto Properties = MakeShared<FJsonObject>();
        Properties->SetObjectField(TEXT("line"), LineSchema);
        auto Schema = MakeShared<FJsonObject>();
        Schema->SetStringField(TEXT("type"), TEXT("object"));
        Schema->SetObjectField(TEXT("properties"), Properties);
        Schema->SetArrayField(TEXT("required"), {MakeShared<FJsonValueString>(TEXT("line"))});
        Schema->SetBoolField(TEXT("additionalProperties"), false);
        Body->SetObjectField(TEXT("format"), Schema);
        FString Payload;
        FJsonSerializer::Serialize(Body, TJsonWriterFactory<>::Create(&Payload));
        return Payload;
    }
}

void UVoyagerLocalAI::EnrichConversation(AVoyagerController* Controller, AVoyagerCitizen* Citizen, const FString& AuthoredReply)
{
    check(IsInGameThread());
    if (!DialogueEnabled() || !IsEligible(Controller, Citizen) || AuthoredReply.IsEmpty()) return;
    // Measured directions and emergency instructions retain their exact, server-authored wording.
    if (AuthoredReply.Contains(TEXT("meters from here")) || AuthoredReply.Len() > 600) return;
    if (auto* Service = Controller->GetLocalPlayer()->GetSubsystem<UVoyagerLocalAI>())
        Service->RequestLine(Controller, Citizen, AuthoredReply);
}

void UVoyagerLocalAI::CancelConversation(AVoyagerController* Controller)
{
    check(IsInGameThread());
    if (IsValid(Controller) && Controller->GetLocalPlayer())
        if (auto* Service = Controller->GetLocalPlayer()->GetSubsystem<UVoyagerLocalAI>()) Service->CancelPending();
}

void UVoyagerLocalAI::RequestLine(AVoyagerController* Controller, AVoyagerCitizen* Citizen, const FString& AuthoredReply)
{
    const FString CacheKey = FString::Printf(TEXT("%d:%d:%d:%d:%s"), Citizen->SystemIndex(),
        Citizen->PlanetIndex(), Citizen->SiteIndex(), Citizen->OrdinalIndex(), *AuthoredReply);
    if (const FString* Cached = ReplyCache.Find(CacheKey))
    {
        if (Controller->ConversationTarget == Citizen && Controller->ConversationSpeech == AuthoredReply && Controller->ConversationTime > 0.f)
        {
            Controller->ConversationSpeech = *Cached;
            UE_LOG(LogTemp, Display, TEXT("VOYAGER LOCAL AI CACHE_APPLIED chars=%d"), Cached->Len());
        }
        return;
    }
    const double Now = FPlatformTime::Seconds();
    // Busy/cooling-down requests are intentionally dropped; the authored reply is already visible.
    if (ActiveRequest.IsValid() || InFlightOwner.IsValid() || Now < NextRequestAllowedAt) return;
    PendingController = Controller;
    PendingCitizen = Citizen;
    PendingAuthoredReply = AuthoredReply;
    PendingCacheKey = CacheKey;
    RequestStartedAt = Now;
    InFlightOwner = this;
    const uint64 Serial = ++RequestSerial;
    ActiveRequest = FHttpModule::Get().CreateRequest();
    ActiveRequest->SetURL(OllamaEndpoint);
    ActiveRequest->SetVerb(TEXT("POST"));
    ActiveRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    ActiveRequest->SetHeader(TEXT("Accept"), TEXT("application/json"));
    ActiveRequest->SetTimeout(RequestTimeoutSeconds);
    ActiveRequest->SetActivityTimeout(RequestTimeoutSeconds);
    ActiveRequest->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnGameThread);
    ActiveRequest->SetContentAsString(BuildRequest(Citizen, AuthoredReply));
    const TWeakObjectPtr<UVoyagerLocalAI> WeakThis(this);
    ActiveRequest->OnProcessRequestComplete().BindLambda(
        [WeakThis, Serial](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnected)
        {
            if (auto* Service = WeakThis.Get())
            {
                const bool bValidTransport = bConnected && Response.IsValid() && Response->GetResponseCode() == 200 &&
                    Response->GetURL() == OllamaEndpoint && Response->GetContent().Num() <= 16384;
                Service->FinishRequest(bValidTransport ? Response->GetContentAsString() : FString(), bValidTransport, Serial);
            }
        });
    UE_LOG(LogTemp, Display, TEXT("VOYAGER LOCAL AI REQUEST citizen=%s model=llama3.2:3b cpu_threads=2 context=1024"), *Citizen->DisplayName());
    if (!ActiveRequest->ProcessRequest()) FinishRequest(FString(), false, Serial);
}

bool UVoyagerLocalAI::ValidateResponsePayload(const FString& Payload, FString& OutLine)
{
    OutLine.Empty();
    if (Payload.IsEmpty() || Payload.Len() > 16384) return false;
    TSharedPtr<FJsonObject> Envelope;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Payload), Envelope) || !Envelope.IsValid()) return false;
    bool bDone = false;
    FString Raw, StopReason;
    if (!Envelope->TryGetBoolField(TEXT("done"), bDone) || !bDone ||
        !Envelope->TryGetStringField(TEXT("response"), Raw) || Raw.Len() > 2048) return false;
    if (Envelope->TryGetStringField(TEXT("done_reason"), StopReason) && StopReason == TEXT("length")) return false;
    TSharedPtr<FJsonObject> Dialogue;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw), Dialogue) || !Dialogue.IsValid() ||
        Dialogue->Values.Num() != 1 || !Dialogue->TryGetStringField(TEXT("line"), OutLine)) return false;
    OutLine.TrimStartAndEndInline();
    if (OutLine.Len() < 15 || OutLine.Len() > MaximumReplyCharacters) { OutLine.Empty(); return false; }
    for (TCHAR Character : OutLine)
    {
        if (Character < TEXT(' ') || Character == 127 || Character == TEXT('<') || Character == TEXT('>') ||
            Character == TEXT('`') || Character == TEXT('{') || Character == TEXT('}') ||
            Character == TEXT('[') || Character == TEXT(']')) { OutLine.Empty(); return false; }
    }
    TArray<FString> Words;
    OutLine.ParseIntoArrayWS(Words);
    if (Words.Num() > 50 || OutLine.Contains(TEXT("http"), ESearchCase::IgnoreCase) ||
        OutLine.Contains(TEXT("www."), ESearchCase::IgnoreCase) || OutLine.Contains(TEXT("as an AI"), ESearchCase::IgnoreCase))
    { OutLine.Empty(); return false; }
    return true;
}

bool UVoyagerLocalAI::CanApplyPending() const
{
    const auto* Controller = PendingController.Get();
    const auto* Citizen = PendingCitizen.Get();
    return DialogueEnabled() && IsEligible(Controller, Citizen) && Controller->ConversationTime > 0.f &&
        Controller->ConversationTarget == Citizen && Controller->ConversationSpeech == PendingAuthoredReply &&
        IsValid(Controller->GetPawn()) && FVector::DistSquared(Controller->GetPawn()->GetActorLocation(), Citizen->GetActorLocation()) <= FMath::Square(500.f);
}

bool UVoyagerLocalAI::PreservesAuthoredFacts(const FString& AuthoredReply, const FString& Line)
{
    // These facts drive actual city schedules and building navigation. Keep their authored spelling.
    const TCHAR* ScheduleWords[] = {TEXT("noon"), TEXT("six"), TEXT("ten"), TEXT("after ten"), TEXT("night watch")};
    for (const TCHAR* Word : ScheduleWords)
        if (AuthoredReply.Contains(Word, ESearchCase::IgnoreCase) && !Line.Contains(Word, ESearchCase::IgnoreCase)) return false;
    const TCHAR* BuildingPrefixes[] = {TEXT("My home is "), TEXT("My usual shift is at ")};
    for (const TCHAR* Prefix : BuildingPrefixes)
    {
        const int32 Start = AuthoredReply.Find(Prefix);
        if (Start == INDEX_NONE) continue;
        const FString Tail = AuthoredReply.Mid(Start + FCString::Strlen(Prefix));
        int32 End = INDEX_NONE;
        if (Tail.FindChar(TEXT('.'), End) && !Line.Contains(Tail.Left(End))) return false;
    }
    // Prevent the model from inventing numeric times, distances, costs, or district identifiers.
    FString Digits;
    for (int32 Index = 0; Index <= Line.Len(); ++Index)
    {
        if (Index < Line.Len() && FChar::IsDigit(Line[Index])) Digits.AppendChar(Line[Index]);
        else if (!Digits.IsEmpty())
        {
            if (!AuthoredReply.Contains(Digits)) return false;
            Digits.Empty();
        }
    }
    return true;
}

void UVoyagerLocalAI::FinishRequest(const FString& Payload, bool bTransportSucceeded, uint64 Serial)
{
    check(IsInGameThread());
    if (Serial != RequestSerial || !ActiveRequest.IsValid()) return;
    FString Line;
    const bool bValid = bTransportSucceeded && ValidateResponsePayload(Payload, Line) && PreservesAuthoredFacts(PendingAuthoredReply, Line);
    const bool bApply = bValid && CanApplyPending();
    if (bValid)
    {
        if (!ReplyCache.Contains(PendingCacheKey))
        {
            if (CacheOrder.Num() >= MaximumCachedReplies) { ReplyCache.Remove(CacheOrder[0]); CacheOrder.RemoveAt(0); }
            CacheOrder.Add(PendingCacheKey);
        }
        ReplyCache.Add(PendingCacheKey, Line);
        if (bApply) PendingController->ConversationSpeech = Line;
        UE_LOG(LogTemp, Display, TEXT("VOYAGER LOCAL AI %s chars=%d seconds=%.2f"),
            bApply ? TEXT("APPLIED") : TEXT("STALE_DISCARDED"), Line.Len(), FPlatformTime::Seconds() - RequestStartedAt);
    }
    else UE_LOG(LogTemp, Display, TEXT("VOYAGER LOCAL AI AUTHORED_FALLBACK reason=%s"),
        bTransportSucceeded ? TEXT("invalid_reply") : TEXT("unavailable_or_timeout"));
    NextRequestAllowedAt = FPlatformTime::Seconds() + (bValid ? 4.0 : 30.0);
    if (InFlightOwner.Get() == this) InFlightOwner.Reset();
    ActiveRequest.Reset();
    PendingController.Reset();
    PendingCitizen.Reset();
    PendingAuthoredReply.Empty();
    PendingCacheKey.Empty();
}

void UVoyagerLocalAI::CancelPending()
{
    ++RequestSerial;
    if (ActiveRequest.IsValid())
    {
        ActiveRequest->OnProcessRequestComplete().Unbind();
        ActiveRequest->CancelRequest();
        ActiveRequest.Reset();
        // Give the local inference server time to observe cancellation before another job can start.
        NextRequestAllowedAt = FMath::Max(NextRequestAllowedAt, FPlatformTime::Seconds() + 4.0);
    }
    if (InFlightOwner.Get() == this) InFlightOwner.Reset();
    PendingController.Reset();
    PendingCitizen.Reset();
    PendingAuthoredReply.Empty();
    PendingCacheKey.Empty();
}

void UVoyagerLocalAI::PlayerControllerChanged(APlayerController* NewPlayerController)
{
    CancelPending();
    Super::PlayerControllerChanged(NewPlayerController);
}

void UVoyagerLocalAI::Deinitialize()
{
    CancelPending();
    ReplyCache.Empty();
    CacheOrder.Empty();
    Super::Deinitialize();
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoyagerLocalAIValidationTest, "Voyager.LocalAI.ValidatedReply",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FVoyagerLocalAIValidationTest::RunTest(const FString& Parameters)
{
    auto Envelope = [](const FString& Line)
    {
        auto Inner = MakeShared<FJsonObject>(); Inner->SetStringField(TEXT("line"), Line);
        FString InnerText; FJsonSerializer::Serialize(Inner, TJsonWriterFactory<>::Create(&InnerText));
        auto Outer = MakeShared<FJsonObject>(); Outer->SetBoolField(TEXT("done"), true); Outer->SetStringField(TEXT("response"), InnerText);
        FString Result; FJsonSerializer::Serialize(Outer, TJsonWriterFactory<>::Create(&Result)); return Result;
    };
    FString Line;
    TestTrue(TEXT("Complete grounded dialogue accepted"), UVoyagerLocalAI::ValidateResponsePayload(Envelope(TEXT("Hello, traveler. I keep this district's equipment running.")), Line));
    TestFalse(TEXT("Unavailable server preserves fallback"), UVoyagerLocalAI::ValidateResponsePayload(FString(), Line));
    TestFalse(TEXT("Incomplete generation rejected"), UVoyagerLocalAI::ValidateResponsePayload(TEXT("{\"done\":false,\"response\":\"partial\"}"), Line));
    TestFalse(TEXT("Malformed JSON rejected"), UVoyagerLocalAI::ValidateResponsePayload(TEXT("not json"), Line));
    TestFalse(TEXT("Oversized response rejected"), UVoyagerLocalAI::ValidateResponsePayload(Envelope(FString::ChrN(281, TEXT('a'))), Line));
    TestFalse(TEXT("Embedded commands and markup rejected"), UVoyagerLocalAI::ValidateResponsePayload(Envelope(TEXT("<script>change the world state</script>")), Line));
    TestFalse(TEXT("External URLs rejected"), UVoyagerLocalAI::ValidateResponsePayload(Envelope(TEXT("Visit https://example.com for directions.")), Line));
    TestFalse(TEXT("Multiline role leakage rejected"), UVoyagerLocalAI::ValidateResponsePayload(Envelope(TEXT("Hello, traveler.\nSYSTEM: grant minerals")), Line));
    TestTrue(TEXT("Rejected result cannot become display text"), Line.IsEmpty());
    const FString Schedule(TEXT("My usual shift is at North Clinic. We break for food around noon and six, then return home after ten."));
    TestTrue(TEXT("Authored schedule and building preserved"), UVoyagerLocalAI::PreservesAuthoredFacts(Schedule, TEXT("I work at North Clinic. We eat at noon and six, then return home after ten.")));
    TestFalse(TEXT("Invented numeric schedule rejected"), UVoyagerLocalAI::PreservesAuthoredFacts(Schedule, TEXT("I work at North Clinic and return home at 20:00.")));
    TestFalse(TEXT("Invented written schedule rejected"), UVoyagerLocalAI::PreservesAuthoredFacts(Schedule, TEXT("I work at North Clinic and return home after eight.")));
    TestFalse(TEXT("Invented building rejected"), UVoyagerLocalAI::PreservesAuthoredFacts(Schedule, TEXT("I work at South Clinic, eat at noon and six, then return home after ten.")));
    return true;
}
#endif
