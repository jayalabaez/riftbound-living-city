#include "RiftEnemy.h"
#include "RiftCharacter.h"
#include "RiftGameMode.h"
#include "RiftWorld.h"
#include "RiftVisual.h"
#include "Components/CapsuleComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/DamageType.h"
#include "Kismet/GameplayStatics.h"
#include "Math/RotationMatrix.h"
#include "Net/UnrealNetwork.h"

ARiftEnemy::ARiftEnemy()
{
    PrimaryActorTick.bCanEverTick = true;
    bReplicates = true;
    SetReplicateMovement(true);
    SetNetUpdateFrequency(25.f);
    SetMinNetUpdateFrequency(10.f);
    Collision = CreateDefaultSubobject<UCapsuleComponent>(TEXT("Collision"));
    SetRootComponent(Collision);
    Collision->InitCapsuleSize(43.f, 75.f);
    Collision->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    Collision->SetCollisionObjectType(ECC_Pawn);
    Collision->SetCollisionResponseToAllChannels(ECR_Block);
    Collision->SetGenerateOverlapEvents(false);
    VisualRoot = CreateDefaultSubobject<USceneComponent>(TEXT("VisualRoot"));
    VisualRoot->SetupAttachment(Collision);
    Tags.Add(TEXT("Enemy"));
}

void ARiftEnemy::BeginPlay()
{
    Super::BeginPlay();
    AvoidanceSign = FMath::RandBool() ? 1.f : -1.f;
    AttackCooldown = FMath::FRandRange(.9f, 2.f);
    MineCooldown = FMath::FRandRange(5.f, 9.f);
    if (HasAuthority()) Health = Archetype == 0 ? 50.f : (Archetype == 1 ? 75.f : 110.f);
    BuildVisuals();
}

void ARiftEnemy::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ARiftEnemy, Archetype);
    DOREPLIFETIME(ARiftEnemy, Health);
    DOREPLIFETIME(ARiftEnemy, bDead);
}

void ARiftEnemy::OnRep_Archetype()
{
    if (HasActorBegunPlay()) BuildVisuals();
}

void ARiftEnemy::BuildVisuals()
{
    for (UStaticMeshComponent* Part : Parts) if (IsValid(Part)) Part->DestroyComponent();
    Parts.Empty();
    auto Mesh = [this](const TCHAR* Name, const TCHAR* Shape, FVector Position,
        FVector Scale, FLinearColor Color, bool Glow = false)
    {
        UStaticMeshComponent* Result = RiftVisual::Mesh(this, VisualRoot, FName(Name), Shape, Position, Scale, Color, Glow);
        Parts.Add(Result);
        return Result;
    };
    const FLinearColor Dark(.025f, .045f, .065f);
    const FLinearColor Cyan(.02f, .9f, 1.f);
    const FLinearColor Amber(1.f, .22f, .015f);
    if (Archetype == 0)
    {
        Collision->SetCapsuleSize(57.f, 58.f);
        Mesh(TEXT("DroneCore"), TEXT("Sphere"), FVector::ZeroVector, FVector(1.0f, .68f, .5f), Dark);
        Mesh(TEXT("DroneArmor"), TEXT("Sphere"), FVector(-9, 0, 8), FVector(.76f, .58f, .28f), FLinearColor(.15f, .25f, .3f));
        Eye = Mesh(TEXT("DroneEye"), TEXT("Sphere"), FVector(44, 0, 2), FVector(.17f, .24f, .2f), Cyan, true);
        Mesh(TEXT("LeftWing"), TEXT("Cube"), FVector(-7, -63, 3), FVector(.6f, .72f, .09f), Dark);
        Mesh(TEXT("RightWing"), TEXT("Cube"), FVector(-7, 63, 3), FVector(.6f, .72f, .09f), Dark);
        Mesh(TEXT("LeftEngine"), TEXT("Cylinder"), FVector(-13, -77, -8), FVector(.27f, .27f, .22f), Cyan, true);
        Mesh(TEXT("RightEngine"), TEXT("Cylinder"), FVector(-13, 77, -8), FVector(.27f, .27f, .22f), Cyan, true);
        Mesh(TEXT("DroneAntenna"), TEXT("Cone"), FVector(-20, 0, 32), FVector(.15f, .15f, .4f), Dark);
    }
    else if (Archetype == 1)
    {
        Collision->SetCapsuleSize(37.f, 80.f);
        Mesh(TEXT("SoldierTorso"), TEXT("Cube"), FVector(0, 0, 3), FVector(.43f, .61f, .64f), FLinearColor(.13f, .2f, .13f));
        Mesh(TEXT("SoldierPlate"), TEXT("Cube"), FVector(24, 0, 7), FVector(.11f, .55f, .45f), Dark);
        Mesh(TEXT("SoldierHead"), TEXT("Sphere"), FVector(0, 0, 51), FVector(.43f, .43f, .4f), Dark);
        Eye = Mesh(TEXT("SoldierVisor"), TEXT("Cube"), FVector(20, 0, 52), FVector(.07f, .33f, .09f), Amber, true);
        Mesh(TEXT("LeftLeg"), TEXT("Cube"), FVector(0, -17, -49), FVector(.23f, .23f, .54f), Dark);
        Mesh(TEXT("RightLeg"), TEXT("Cube"), FVector(0, 17, -49), FVector(.23f, .23f, .54f), Dark);
        Mesh(TEXT("LeftBoot"), TEXT("Cube"), FVector(7, -17, -73), FVector(.35f, .26f, .12f), Dark);
        Mesh(TEXT("RightBoot"), TEXT("Cube"), FVector(7, 17, -73), FVector(.35f, .26f, .12f), Dark);
        Mesh(TEXT("LeftShoulder"), TEXT("Sphere"), FVector(0, -43, 18), FVector(.31f, .3f, .32f), FLinearColor(.26f, .31f, .15f));
        Mesh(TEXT("RightShoulder"), TEXT("Sphere"), FVector(0, 43, 18), FVector(.31f, .3f, .32f), FLinearColor(.26f, .31f, .15f));
        Mesh(TEXT("Rifle"), TEXT("Cube"), FVector(46, 29, 1), FVector(.91f, .13f, .18f), Dark);
        Mesh(TEXT("RifleMuzzle"), TEXT("Sphere"), FVector(88, 29, 1), FVector(.09f), Amber, true);
        Mesh(TEXT("MinePack"), TEXT("Cube"), FVector(-29, 0, 5), FVector(.25f, .5f, .5f), FLinearColor(.32f, .29f, .09f));
    }
    else
    {
        Collision->SetCapsuleSize(67.f, 80.f);
        const FLinearColor Hide(.25f, .1f, .055f);
        Mesh(TEXT("BeastBody"), TEXT("Sphere"), FVector(-12, 0, -1), FVector(1.9f, .87f, 1.05f), Hide);
        Mesh(TEXT("BeastNeck"), TEXT("Sphere"), FVector(49, 0, 29), FVector(.8f, .61f, .76f), Hide);
        Mesh(TEXT("BeastHead"), TEXT("Sphere"), FVector(86, 0, 47), FVector(.97f, .65f, .49f), FLinearColor(.35f, .18f, .085f));
        Mesh(TEXT("BeastJaw"), TEXT("Cube"), FVector(110, 0, 26), FVector(.73f, .49f, .13f), FLinearColor(.14f, .035f, .015f));
        Eye = Mesh(TEXT("BeastEyeLeft"), TEXT("Sphere"), FVector(99, -28, 57), FVector(.1f), FLinearColor(1.f, .45f, .015f), true);
        Mesh(TEXT("BeastEyeRight"), TEXT("Sphere"), FVector(99, 28, 57), FVector(.1f), FLinearColor(1.f, .45f, .015f), true);
        Mesh(TEXT("BeastTail"), TEXT("Cone"), FVector(-140, 0, 3), FVector(.6f, .6f, 2.f), Hide)->SetRelativeRotation(FRotator(-90, 0, 0));
        for (int32 Side = -1; Side <= 1; Side += 2)
        {
            Mesh(*FString::Printf(TEXT("BeastThigh%d"), Side), TEXT("Sphere"), FVector(-27, Side * 41, -28), FVector(.6f, .37f, .74f), Hide);
            Mesh(*FString::Printf(TEXT("BeastFoot%d"), Side), TEXT("Cube"), FVector(3, Side * 42, -70), FVector(.71f, .29f, .19f), FLinearColor(.13f, .08f, .055f));
            Mesh(*FString::Printf(TEXT("BeastArm%d"), Side), TEXT("Cone"), FVector(53, Side * 41, -1), FVector(.19f, .19f, .55f), Hide)->SetRelativeRotation(FRotator(35, 0, 0));
        }
        for (int32 Index = 0; Index < 5; ++Index)
        {
            Mesh(*FString::Printf(TEXT("Spine%d"), Index), TEXT("Cone"), FVector(-75 + Index * 24, 0, 49), FVector(.19f, .18f, .4f), FLinearColor(.7f, .38f, .15f));
        }
    }
    AttackBeam = Mesh(TEXT("AttackBeam"), TEXT("Cylinder"), FVector::ZeroVector, FVector(.025f), Archetype == 0 ? Cyan : Amber, true);
    ImpactFlash = Mesh(TEXT("ImpactFlash"), TEXT("Sphere"), FVector::ZeroVector, FVector(.12f), Archetype == 0 ? Cyan : Amber, true);
    AttackBeam->SetVisibility(false);
    ImpactFlash->SetVisibility(false);
    bVisualsBuilt = true;
    if (bDead) OnRep_Dead();
}

ARiftCharacter* ARiftEnemy::FindTarget() const
{
    ARiftCharacter* Best = nullptr;
    float BestDistance = MAX_flt;
    for (TActorIterator<ARiftCharacter> It(GetWorld()); It; ++It)
    {
        if (It->Health <= 0.f || !It->GetController()) continue;
        const float Distance = FVector::DistSquared(It->GetActorLocation(), GetActorLocation());
        if (Distance < BestDistance) { Best = *It; BestDistance = Distance; }
    }
    return Best;
}

bool ARiftEnemy::CanSee(const ARiftCharacter* Target) const
{
    FHitResult Hit;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(RiftEnemyVision), false, this);
    const bool bHit = GetWorld()->LineTraceSingleByChannel(Hit, GetActorLocation() + FVector(0, 0, 25),
        Target->GetActorLocation() + FVector(0, 0, 20), ECC_Visibility, Query);
    return !bHit || Hit.GetActor() == Target;
}

void ARiftEnemy::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    Age += DeltaSeconds;
    if (EffectTime > 0.f)
    {
        EffectTime -= DeltaSeconds;
        if (EffectTime <= 0.f && AttackBeam && ImpactFlash)
        {
            AttackBeam->SetVisibility(false);
            ImpactFlash->SetVisibility(false);
        }
    }
    if (bDead)
    {
        DeathTime += DeltaSeconds;
        VisualRoot->SetRelativeRotation(FRotator(DeathTime * 140.f, DeathTime * 100.f, 0));
        VisualRoot->SetRelativeScale3D(FVector(FMath::Max(.01f, 1.f - DeathTime * 1.8f)));
        return;
    }
    if (VisualRoot)
    {
        const float Bob = Archetype == 0 ? FMath::Sin(Age * 4.f) * 10.f : FMath::Sin(Age * 10.f) * 3.f;
        VisualRoot->SetRelativeLocation(FVector(0, 0, Bob));
        if (Archetype == 2) VisualRoot->SetRelativeRotation(FRotator(0, 0, FMath::Sin(Age * 7.f) * 3.f));
    }
    if (HasAuthority()) SimulateEnemy(DeltaSeconds);
}

void ARiftEnemy::SimulateEnemy(float DeltaSeconds)
{
    const ARiftGameState* State = GetWorld()->GetGameState<ARiftGameState>();
    if (State && State->Phase != 2) return;
    AttackCooldown -= DeltaSeconds;
    MineCooldown -= DeltaSeconds;
    ObstacleCooldown -= DeltaSeconds;
    ARiftCharacter* Target = FindTarget();
    if (!Target) return;
    FVector ToTarget = Target->GetActorLocation() - GetActorLocation();
    ToTarget.Z = 0;
    const float Distance = ToTarget.Size();
    FVector Direction = ToTarget.GetSafeNormal();
    const FRotator Facing = Direction.Rotation();
    SetActorRotation(FMath::RInterpTo(GetActorRotation(), Facing, DeltaSeconds, 7.f));
    const float Range = Archetype == 2 ? 185.f : (Archetype == 1 ? 1050.f : 900.f);
    const bool bVisible = CanSee(Target);
    const float DesiredDistance = Archetype == 2 ? 145.f : (Archetype == 1 ? 570.f : 610.f);
    const float Speed = Archetype == 2 ? 290.f : (Archetype == 1 ? 220.f : 265.f);
    if (Distance > DesiredDistance || !bVisible)
    {
        FVector Separation = FVector::ZeroVector;
        for (TActorIterator<ARiftEnemy> It(GetWorld()); It; ++It)
        {
            if (*It == this || It->bDead) continue;
            FVector Away = GetActorLocation() - It->GetActorLocation();
            Away.Z = 0;
            const float SeparationDistance = Away.Size();
            if (SeparationDistance > 1.f && SeparationDistance < 170.f)
                Separation += Away / SeparationDistance * (1.f - SeparationDistance / 170.f) * .85f;
        }
        Direction = (Direction + Separation).GetSafeNormal();
        FHitResult Hit;
        const FVector Move = Direction * Speed * DeltaSeconds;
        AddActorWorldOffset(Move, true, &Hit);
        if (Hit.bBlockingHit)
        {
            FVector Tangent = FVector::VectorPlaneProject(Move, Hit.Normal);
            if (Tangent.SizeSquared() < Move.SizeSquared() * .15f)
                Tangent = FVector(-Direction.Y, Direction.X, 0) * AvoidanceSign * Speed * DeltaSeconds;
            Tangent.Z = 0;
            FHitResult SlideHit;
            AddActorWorldOffset(Tangent, true, &SlideHit);
            if (SlideHit.bBlockingHit && FMath::Fmod(Age, 1.2f) < DeltaSeconds) AvoidanceSign *= -1.f;
            if (Archetype == 2 && ObstacleCooldown <= 0.f && Cast<ARiftProp>(Hit.GetActor()))
            {
                ObstacleCooldown = .9f;
                UGameplayStatics::ApplyDamage(Hit.GetActor(), 65.f, nullptr, this, UDamageType::StaticClass());
            }
        }
    }
    if (Archetype == 0)
    {
        const float DesiredHeight = 205.f + FMath::Sin(Age * 1.6f) * 35.f;
        const float HeightStep = FMath::Clamp(DesiredHeight - GetActorLocation().Z, -80.f * DeltaSeconds, 80.f * DeltaSeconds);
        AddActorWorldOffset(FVector(0, 0, HeightStep), true);
    }
    if (Distance <= Range && bVisible && AttackCooldown <= 0.f)
    {
        AttackCooldown = Archetype == 2 ? 1.35f : (Archetype == 1 ? 1.65f : 1.9f);
        MulticastAttack(Target->GetActorLocation() + FVector(0, 0, 16));
        UGameplayStatics::ApplyDamage(Target, Archetype == 2 ? 14.f : (Archetype == 1 ? 6.f : 5.f),
            nullptr, this, UDamageType::StaticClass());
    }
    if (Archetype == 1 && MineCooldown <= 0.f && Distance > 330.f)
    {
        MineCooldown = FMath::FRandRange(10.f, 14.f);
        int32 MineCount = 0;
        for (TActorIterator<ARiftMine> It(GetWorld()); It; ++It) ++MineCount;
        if (MineCount < 16)
        {
            FActorSpawnParameters Params;
            Params.Owner = this;
            Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            FVector Position = GetActorLocation() - GetActorForwardVector() * 85.f;
            Position.Z = 12.f;
            GetWorld()->SpawnActor<ARiftMine>(Position, FRotator::ZeroRotator, Params);
        }
    }
}

void ARiftEnemy::MulticastAttack_Implementation(FVector_NetQuantize Target)
{
    if (!AttackBeam || !ImpactFlash || bDead) return;
    const FVector Start = GetActorLocation() + GetActorForwardVector() * 55.f + FVector(0, 0, 20);
    const FVector Delta = FVector(Target) - Start;
    AttackBeam->SetWorldLocation(Start + Delta * .5f);
    AttackBeam->SetWorldRotation(FRotationMatrix::MakeFromZ(Delta.GetSafeNormal()).Rotator());
    AttackBeam->SetWorldScale3D(FVector(.018f, .018f, Delta.Size() / 100.f));
    AttackBeam->SetVisibility(Archetype != 2);
    ImpactFlash->SetWorldLocation(Target);
    ImpactFlash->SetWorldScale3D(FVector(Archetype == 2 ? .4f : .13f));
    ImpactFlash->SetVisibility(true);
    EffectTime = .10f;
}

float ARiftEnemy::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
    AController* EventInstigator, AActor* DamageCauser)
{
    if (!HasAuthority() || bDead || DamageAmount <= 0.f) return 0.f;
    const float Applied = FMath::Min(Health, DamageAmount);
    Health = FMath::Max(0.f, Health - DamageAmount);
    if (Health <= 0.f)
    {
        bDead = true;
        OnRep_Dead();
        ForceNetUpdate();
        SetLifeSpan(.8f);
        if (ARiftGameMode* Mode = GetWorld()->GetAuthGameMode<ARiftGameMode>()) Mode->EnemyKilled(EventInstigator);
    }
    return Applied;
}

void ARiftEnemy::OnRep_Dead()
{
    if (!bDead) return;
    Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    if (AttackBeam) AttackBeam->SetVisibility(false);
    if (ImpactFlash) ImpactFlash->SetVisibility(false);
    for (UStaticMeshComponent* Part : Parts)
    {
        if (Part) Part->SetMaterial(0, RiftVisual::Material(this, FLinearColor(.1f, .9f, 1.f), true));
    }
}

ARiftMine::ARiftMine()
{
    PrimaryActorTick.bCanEverTick = true;
    bReplicates = true;
    Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
    SetRootComponent(Collision);
    Collision->InitSphereRadius(28.f);
    Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    Collision->SetCollisionObjectType(ECC_WorldDynamic);
    Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
    Collision->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
    Tags.Add(TEXT("Mine"));
}

void ARiftMine::BeginPlay()
{
    Super::BeginPlay();
    RiftVisual::Mesh(this, Collision, TEXT("MineBody"), TEXT("Cylinder"), FVector::ZeroVector,
        FVector(.58f, .58f, .12f), FLinearColor(.08f, .09f, .045f));
    Indicator = RiftVisual::Mesh(this, Collision, TEXT("Indicator"), TEXT("Sphere"), FVector(0, 0, 10),
        FVector(.15f), FLinearColor(1.f, .08f, .01f), true);
    WarningRing = RiftVisual::Mesh(this, Collision, TEXT("Warning"), TEXT("Cylinder"), FVector(0, 0, -8),
        FVector(3.f, 3.f, .008f), FLinearColor(.42f, .07f, .008f), true);
    Blast = RiftVisual::Mesh(this, Collision, TEXT("Blast"), TEXT("Sphere"), FVector::ZeroVector,
        FVector(.01f), FLinearColor(1.f, .24f, .015f), true);
    Blast->SetVisibility(false);
    if (HasAuthority()) SetLifeSpan(35.f);
    if (bExploded) OnRep_Exploded();
}

void ARiftMine::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ARiftMine, bExploded);
}

void ARiftMine::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    Age += DeltaSeconds;
    if (bExploded)
    {
        BlastAge += DeltaSeconds;
        if (Blast) Blast->SetRelativeScale3D(FVector(.1f + BlastAge * 17.f));
        return;
    }
    if (Indicator) Indicator->SetVisibility(FMath::Sin(Age * (Age < 2.f ? 5.f : 14.f)) > -.35f);
    if (WarningRing) WarningRing->SetRelativeScale3D(FVector(2.85f + .15f * FMath::Sin(Age * 5.f), 2.85f + .15f * FMath::Sin(Age * 5.f), .008f));
    if (HasAuthority() && Age > 2.f)
    {
        for (TActorIterator<ARiftCharacter> It(GetWorld()); It; ++It)
        {
            if (It->Health > 0.f && FVector::Dist2D(It->GetActorLocation(), GetActorLocation()) < 150.f)
            {
                Explode();
                break;
            }
        }
    }
}

float ARiftMine::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
    AController* EventInstigator, AActor* DamageCauser)
{
    if (HasAuthority() && DamageAmount > 0.f && !bExploded) Explode();
    return DamageAmount;
}

void ARiftMine::Explode()
{
    if (!HasAuthority() || bExploded) return;
    bExploded = true;
    OnRep_Exploded();
    ForceNetUpdate();
    SetLifeSpan(.3f);
    for (TActorIterator<ARiftCharacter> It(GetWorld()); It; ++It)
    {
        const float Distance = FVector::Dist2D(It->GetActorLocation(), GetActorLocation());
        if (Distance < 260.f && It->Health > 0.f)
        {
            FHitResult Hit;
            FCollisionQueryParams Query(SCENE_QUERY_STAT(RiftMineBlast), false, this);
            const bool bHit = GetWorld()->LineTraceSingleByChannel(Hit, GetActorLocation() + FVector(0, 0, 45),
                It->GetActorLocation(), ECC_Visibility, Query);
            if (!bHit || Hit.GetActor() == *It)
                UGameplayStatics::ApplyDamage(*It, FMath::Lerp(24.f, 8.f, Distance / 260.f), nullptr, this, UDamageType::StaticClass());
        }
    }
}

void ARiftMine::OnRep_Exploded()
{
    if (!bExploded) return;
    Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    if (Indicator) Indicator->SetVisibility(false);
    if (WarningRing) WarningRing->SetVisibility(false);
    if (Blast) Blast->SetVisibility(true);
}
