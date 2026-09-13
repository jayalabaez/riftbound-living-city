#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "VoyagerCharacter.generated.h"
class UCameraComponent;
class UAudioComponent;
class AVoyagerCitizen;
UCLASS()
class RIFTBOUND_API AVoyagerCharacter : public ACharacter
{
    GENERATED_BODY()
public:
    AVoyagerCharacter();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void FaceRotation(FRotator NewControlRotation,float DeltaTime=0.f) override;
    virtual FVector GetPawnViewLocation() const override;
    virtual void SetupPlayerInputComponent(UInputComponent* Input) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UCameraComponent> Camera;
    UPROPERTY(Replicated) float Health=100;
    float ScanPulse=0;
    float MiningFeedback=0;
    bool IsBodycamEnabled() const { return bBodycam; }
    void ToggleBodycam();
    UFUNCTION(Server,Reliable) void ServerInteract();
    UFUNCTION(Server,Reliable) void ServerTalk(int32 Topic);
    UFUNCTION(Server,Reliable) void ServerEndTalk();
    AVoyagerCitizen* FocusedCitizen() const;
    UFUNCTION(Server,Reliable) void ServerScan();
    UFUNCTION(Server,Reliable) void ServerMine(FVector_NetQuantizeNormal Direction);
    UFUNCTION(Server,Reliable) void ServerSprint(bool bValue);
    UFUNCTION(NetMulticast,Unreliable) void MiningBeam(FVector End,bool Success);
private:
    UPROPERTY() TObjectPtr<USceneComponent> Tool;
    TWeakObjectPtr<AVoyagerCitizen> TalkPartner;
    bool bMining=false;
    bool bSprinting=false;
    bool bBodycam=true;
    float CameraStride=0.f;
    float NextShot=0,LastServerShot=-10,LastScan=-10,LastTalk=-10;
    void Forward(float Value);
    void Right(float Value);
    void Turn(float Value);
    void Look(float Value);
    void StartMine();
    void StopMine();
    void Interact();
    void Scan();
    void Sprint();
    void StopSprint();
    bool CanAct() const;
};

UCLASS()
class RIFTBOUND_API AVoyagerController : public APlayerController
{
    GENERATED_BODY()
public:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void UpdateRotation(float DeltaTime) override;
    virtual void SetupInputComponent() override;
    void ToggleMenu();
    bool bMenuVisible=false;
    FString Notice;
    float NoticeTime=0;
    UFUNCTION(Client,Reliable) void Notify(const FString& Message);
    UFUNCTION(Client,Reliable) void ShowConversation(AVoyagerCitizen* Citizen,const FString& Speech);
    TWeakObjectPtr<AVoyagerCitizen> ConversationTarget;
    FString ConversationSpeech;
    float ConversationTime=0;
    UFUNCTION(Server,Reliable) void ServerSave();
    UFUNCTION(Server,Reliable) void ServerUpgrade();
private:
    TSharedPtr<class SWidget> MenuWidget;
    TSharedPtr<class SEditableTextBox> AddressBox;
    void ShowMenu();
    void HideMenu();
    void SaveInput();
    void UpgradeInput();
    void TalkGreeting();
    void TalkLife();
    void TalkDirections();
    void CloseConversation();
    float TestTime=0;
    float StageStarted=0;
    int32 TestStage=0;
    float LastProbe=0;
    FVector TestStart=FVector::ZeroVector;
    FVector TestLastLocation=FVector::ZeroVector;
    TWeakObjectPtr<APawn> TestFlightPawn;
    int32 TestFlightEpoch=0,TestWorldRevision=0,TestCaptures=0;
    double TestMaxFlightStep=0;
    double TestLastSimSeconds=0;
    double PerformanceWindowStart=0,PreviousFrameTime=0,WorstFrameMs=0;
    uint32 PerformanceFrames=0;
    float PreviousProbeDelta=0;
    void RunProbe(float DeltaSeconds);
    void RunSurfaceAudit(float DeltaSeconds);
    void RunLifeAudit(float DeltaSeconds);
    void RunCityAudit(float DeltaSeconds);
    void RunCityNetworkAudit(float DeltaSeconds);
    TMap<TWeakObjectPtr<AActor>,FVector> LifeAuditPositions;
    TWeakObjectPtr<AVoyagerCitizen> CityAuditCitizen;
    int32 CityAuditStep=0;
};
