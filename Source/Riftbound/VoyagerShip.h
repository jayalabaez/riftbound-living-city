#pragma once
#include "CoreMinimal.h"
#include "VoyagerShipVisuals.h"
#include "GameFramework/Pawn.h"
#include "VoyagerShip.generated.h"

class UCameraComponent;
class USpringArmComponent;
class USphereComponent;
class UStaticMeshComponent;

// Inputs are retained until acknowledged. Re-sending recent moves recovers dropped
// unreliable packets; sequence numbers prevent the server applying a move twice.
USTRUCT()
struct FVoyagerFlightMove
{
    GENERATED_BODY()
    UPROPERTY() uint32 Sequence=0;
    UPROPERTY() int32 Epoch=0;
    UPROPERTY() float Seconds=0;
    UPROPERTY() float Throttle=0;
    UPROPERTY() float Strafe=0;
    UPROPERTY() float Lift=0;
    UPROPERTY() float Yaw=0;
    UPROPERTY() float Pitch=0;
    UPROPERTY() bool bBoost=false;
    UPROPERTY() bool bPaused=false;
    UPROPERTY() bool bCruiseObserved=false;
};

USTRUCT()
struct FVoyagerFlightState
{
    GENERATED_BODY()
    UPROPERTY() FVector Location=FVector::ZeroVector;
    UPROPERTY() FVector Velocity=FVector::ZeroVector;
    UPROPERTY() FRotator Rotation=FRotator::ZeroRotator;
    UPROPERTY() float Speed=0;
    UPROPERTY() uint32 Acknowledged=0;
    UPROPERTY() int32 Epoch=0;
    UPROPERTY() bool bLanded=true;
    UPROPERTY() bool bCruising=false;
    UPROPERTY() int32 CruiseLeg=0;
    UPROPERTY() TArray<FVector> CruiseWaypoints;
};

USTRUCT()
struct FVoyagerPiratePose
{
    GENERATED_BODY()
    UPROPERTY() FVector Location=FVector::ZeroVector;
    UPROPERTY() FRotator Rotation=FRotator::ZeroRotator;
};

UCLASS()
class RIFTBOUND_API AVoyagerShip : public APawn
{
    GENERATED_BODY()
public:
    AVoyagerShip();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void SetupPlayerInputComponent(UInputComponent* Input) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    virtual float TakeDamage(float Damage,const FDamageEvent& Event,AController* DamageInstigator,AActor* Causer) override;
    virtual FVector GetVelocity() const override { return FlightVelocity; }
    UPROPERTY(VisibleAnywhere) TObjectPtr<UCameraComponent> Camera;
    UPROPERTY(Replicated) float Hull=100;
    UPROPERTY(Replicated) float Shield=100;
    UPROPERTY(Replicated) float Speed=0;
    UPROPERTY(Replicated) bool bLanded=true;
    UPROPERTY(Replicated) bool bCruising=false;
    UPROPERTY(Replicated) int32 TargetPlanet=0;
    float HitFeedback=0;
    float DamageFeedback=0;
    void SetParked(bool bParked);
    void ResetFlight(FVector Location,FRotator Rotation,bool bParked);
    double SurfaceAltitude() const;
    int32 NearestPlanetIndex() const;
    float AtmosphericDensity() const;
    void StartCruise(int32 Planet);
    void CancelCruise();
    int32 GetFlightEpoch() const { return FlightEpoch; }
    // Cumulative accepted simulation time; authority does not replay moves.
    double GetSimulatedFlightSeconds() const { return SimulatedFlightSeconds; }
    void SetFlightTestInput(float Throttle,float Lift,bool bBoost);
    void ClearFlightTestInput();
    UFUNCTION(Server,Unreliable) void ServerFlightInputs(const TArray<FVoyagerFlightMove>& Moves);
    UFUNCTION(Server,Reliable) void ServerInteract();
    UFUNCTION(Server,Reliable) void ServerSelectTarget();
    UFUNCTION(Server,Reliable) void ServerWarp();
    UFUNCTION(Server,Reliable) void ServerNextSystem();
    UFUNCTION(Server,Unreliable) void ServerShoot();
    UFUNCTION(NetMulticast,Unreliable) void LaserEffect(FVector End,bool bHit);
    UFUNCTION(NetMulticast,Unreliable) void DamageEffect(bool bDestroyed);
    UFUNCTION(Client,Unreliable) void ConfirmHit();
private:
    UPROPERTY(VisibleAnywhere) TObjectPtr<USphereComponent> Collision;
    VoyagerShipVisuals::FAssembly ShipVisuals;
    UPROPERTY() TObjectPtr<USceneComponent> VisualRoot;
    UPROPERTY() TObjectPtr<USpringArmComponent> SpringArm;
    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Engines;
    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> LaserBeams;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> Impact;
    UPROPERTY(ReplicatedUsing=OnRep_FlightState) FVoyagerFlightState FlightState;
    UFUNCTION() void OnRep_FlightState();
    TArray<FVoyagerFlightMove> PendingMoves;
    TArray<FVector> CruiseWaypoints;
    int32 CruiseLeg=0;
    FVector FlightVelocity=FVector::ZeroVector;
    float ThrottleInput=0,StrafeInput=0,LiftInput=0,MouseYaw=0,MousePitch=0;
    float InputSendTime=0,StateSendTime=0,NextShot=0,LastServerShot=-10,LastDamage=-10;
    float EffectsRemaining=0,Age=0,VisualBank=0;
    float SimulationAccumulator=0,PendingYaw=0,PendingPitch=0,NetDiagnosticTime=0;
    double LastInputPacketTime=0;
    double SimulatedFlightSeconds=0;
    float SimulationCredit=.2f;
    uint32 LocalSequence=0,LastProcessedSequence=0;
    int32 FlightEpoch=0;
    bool bBoostInput=false,bFiring=false,bRecovering=false;
    bool bTestInput=false,bTestBoost=false;
    float TestThrottle=0,TestLift=0;
    void BuildVisuals();
    void SimulateMove(const FVoyagerFlightMove& Move,bool bPermitTravel);
    void SimulateCruise(float DeltaSeconds,bool bAuthorityEffects);
    void PublishFlightState();
    bool CanAct() const;
    void Forward(float Value);
    void Right(float Value);
    void Lift(float Value);
    void Turn(float Value);
    void Look(float Value);
    void Boost();
    void StopBoost();
    void StartFire();
    void StopFire();
    void Interact();
    void SelectTarget();
    void Warp();
    void NextSystem();
};

UCLASS()
class RIFTBOUND_API AVoyagerPirate : public AActor
{
    GENERATED_BODY()
public:
    AVoyagerPirate();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    virtual float TakeDamage(float Damage,const FDamageEvent& Event,AController* DamageInstigator,AActor* Causer) override;
    UPROPERTY(Replicated) float Hull=60;
    UFUNCTION(NetMulticast,Unreliable) void BeamEffect(FVector End,bool bWarning);
    UFUNCTION(NetMulticast,Reliable) void DeathEffect();
private:
    UPROPERTY() TObjectPtr<USphereComponent> Collision;
    VoyagerShipVisuals::FAssembly ShipVisuals;
    UPROPERTY() TObjectPtr<USceneComponent> VisualRoot;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> Beam;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> Engine;
    UPROPERTY() TWeakObjectPtr<AVoyagerShip> AttackTarget;
    UPROPERTY(ReplicatedUsing=OnRep_Pose) FVoyagerPiratePose Pose;
    UFUNCTION() void OnRep_Pose();
    float Age=0,FireTime=3,WarningTime=0,EffectRemaining=0,OrbitPhase=0;
    FVector AimPoint=FVector::ZeroVector;
    AVoyagerShip* FindTarget() const;
};
