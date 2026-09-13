#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "VoyagerCityLife.h"
#include "VoyagerCharacter.generated.h"
class UCameraComponent;
class UAudioComponent;
class AVoyagerCitizen;
class AVoyagerAnimal;
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
    virtual float TakeDamage(float Damage,const FDamageEvent& Event,AController* DamageInstigator,AActor* Causer) override;
    UPROPERTY(Replicated) bool bWeaponMode=false;
    void ToggleWeapon();
    UFUNCTION(Server,Reliable) void ServerToggleWeapon();
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
    AVoyagerAnimal* FocusedAnimal() const;
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
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    UPROPERTY(Replicated) int32 Magazine=24;
    UPROPERTY(Replicated) float ReloadRemaining=0.f;
    UFUNCTION(Server,Reliable) void ServerReload();
    UFUNCTION(Server,Reliable) void ServerSurrender();
    void ToggleMenu();
    void CycleGraphics();
    void ApplyGraphics(int32 Level,bool bSave);
    FString GraphicsLabel() const;
    int32 GraphicsLevel=2;
    bool bMenuVisible=false;
    bool bCityPhoneVisible=false;
    bool bBackpackVisible=false;
    int32 CityPhonePage=0;
    int32 CityGuideTarget=1;
    UPROPERTY() FVoyagerCityLifeView CityLifeView;
    void ToggleCityPhone();
    void ToggleBackpack();
    UFUNCTION(Server,Reliable) void ServerSurvivalAction(uint8 Action);
    void CityPhonePreviousPage();
    void CityPhoneNextPage();
    void CityPhoneAction(int32 Number);
    void CycleCityGuide();
    UFUNCTION(Client,Reliable) void ClientReceiveCityLife(const FVoyagerCityLifeView& View);
    UFUNCTION(Server,Reliable) void ServerCityAction(uint8 Action,int32 Argument);
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
    void CityPhoneFour();
    void CityPhoneFive();
    void CityPhoneSix();
    void CityPhoneSeven();
    void CityPhoneEight();
    void CloseCityPhone();
    TWeakObjectPtr<APawn> CityPhonePawn;
    float LastSurvivalAction=-10.f;
    float TestTime=0;
    float StageStarted=0;
    float LandingSettledAt=0;
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
