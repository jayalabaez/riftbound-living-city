#include "RiftWorld.h"
#include "RiftGameMode.h"
#include "RiftVisual.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/AudioComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "Sound/SoundBase.h"

namespace
{
    void SolidCollision(UPrimitiveComponent* Component)
    {
        Component->SetNetAddressable();
        Component->SetCollisionProfileName(TEXT("BlockAll"));
        Component->SetCollisionObjectType(ECC_WorldStatic);
        Component->SetCanEverAffectNavigation(false);
    }

    UStaticMeshComponent* Part(AActor* Owner, const TCHAR* Name, const TCHAR* Shape,
        FVector Location, FVector Scale, FLinearColor Color, bool bGlow = false, bool bCollision = false)
    {
        UStaticMeshComponent* Component = RiftVisual::Mesh(Owner, Owner->GetRootComponent(), FName(Name), Shape, Location, Scale, Color, bGlow);
        if (bCollision) SolidCollision(Component);
        else Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        return Component;
    }

    template<typename T> T* Scene(AActor* Owner, const TCHAR* Name)
    {
        T* Component = NewObject<T>(Owner, FName(Name));
        Component->SetNetAddressable();
        Owner->AddInstanceComponent(Component);
        Component->SetupAttachment(Owner->GetRootComponent());
        Component->RegisterComponent();
        return Component;
    }

    UInstancedStaticMeshComponent* Instances(AActor* Owner, const TCHAR* Name, const TCHAR* Shape, FLinearColor Color, bool bCollision = false)
    {
        UInstancedStaticMeshComponent* Component = Scene<UInstancedStaticMeshComponent>(Owner, Name);
        const FString Path = FString::Printf(TEXT("/Engine/BasicShapes/%s.%s"), Shape, Shape);
        Component->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, *Path));
        Component->SetMaterial(0, RiftVisual::Material(Owner, Color, false));
        if (bCollision) SolidCollision(Component);
        else Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        return Component;
    }

    void Instance(UInstancedStaticMeshComponent* Component, FVector Location, FVector Scale, FRotator Rotation = FRotator::ZeroRotator)
    {
        Component->AddInstance(FTransform(Rotation, Location, Scale));
    }

    UPointLightComponent* Lamp(AActor* Owner, const TCHAR* Name, FVector Location, FLinearColor Color, float Intensity, float Radius)
    {
        UPointLightComponent* Light = Scene<UPointLightComponent>(Owner, Name);
        Light->SetRelativeLocation(Location);
        Light->SetLightColor(Color);
        Light->SetIntensityUnits(ELightUnits::Unitless);
        Light->SetIntensity(Intensity);
        Light->SetAttenuationRadius(Radius);
        Light->SetCastShadows(false);
        return Light;
    }
}

ARiftWorld::ARiftWorld()
{
    bReplicates = true;
    bAlwaysRelevant = true;
    PrimaryActorTick.bCanEverTick = false;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("WorldRoot")));
    GetRootComponent()->SetMobility(EComponentMobility::Static);
}

void ARiftWorld::BeginPlay()
{
    Super::BeginPlay();
    BuildLighting();
    BuildForest();
    BuildCamp();
    TArray<UStaticMeshComponent*> WorldMeshes;
    GetComponents<UStaticMeshComponent>(WorldMeshes);
    for(UStaticMeshComponent* Mesh : WorldMeshes) Mesh->SetMobility(EComponentMobility::Static);
    if (GetNetMode() != NM_DedicatedServer)
    {
        if (USoundBase* Ambient = LoadObject<USoundBase>(nullptr, TEXT("/Game/Audio/S_Ambient.S_Ambient")))
            UGameplayStatics::SpawnSound2D(this, Ambient, .42f);
    }
    if (HasAuthority())
    {
        GetWorld()->SpawnActor<ARiftUFO>(ARiftUFO::StaticClass(), FVector(1800.f, 0.f, 1000.f), FRotator::ZeroRotator);
    }
}

void ARiftWorld::BuildLighting()
{
    auto* Atmosphere = Scene<USkyAtmosphereComponent>(this, TEXT("DuskAtmosphere"));
    Atmosphere->SetSkyLuminanceFactor(FLinearColor(.5f, .6f, .9f));
    auto* Sun = Scene<UDirectionalLightComponent>(this, TEXT("AmberSun"));
    Sun->SetRelativeRotation(FRotator(-22.f, -55.f, 0.f));
    Sun->SetLightColor(FLinearColor(1.f, .72f, .49f));
    Sun->SetIntensity(4.5f);
    Sun->SetAtmosphereSunLight(true);
    Sun->SetCastShadows(true);
    auto* Fill = Scene<UDirectionalLightComponent>(this, TEXT("BlueFill"));
    Fill->SetRelativeRotation(FRotator(-48.f, 125.f, 0.f));
    Fill->SetLightColor(FLinearColor(.43f, .66f, 1.f));
    Fill->SetIntensity(1.8f);
    Fill->SetCastShadows(false);
    auto* Sky = Scene<USkyLightComponent>(this, TEXT("AmbientSky"));
    Sky->SetIntensity(1.5f);
    Sky->SetLightColor(FLinearColor(.65f, .75f, 1.f));
    Sky->SetRealTimeCapture(true);
    auto* Fog = Scene<UExponentialHeightFogComponent>(this, TEXT("ForestMist"));
    Fog->SetFogDensity(.009f);
    Fog->SetFogHeightFalloff(.24f);
    Fog->SetFogInscatteringColor(FLinearColor(.055f, .09f, .15f));
    Fog->SetStartDistance(1200.f);
    Fog->SetVolumetricFog(false);

    auto* Post = Scene<UPostProcessComponent>(this, TEXT("ArenaGrade"));
    Post->bUnbound = true;
    Post->Settings.bOverride_AutoExposureMethod = true;
    Post->Settings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
    Post->Settings.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
    Post->Settings.AutoExposureApplyPhysicalCameraExposure = false;
    Post->Settings.bOverride_AutoExposureBias = true;
    Post->Settings.AutoExposureBias = .4f;
    Post->Settings.bOverride_BloomIntensity = true;
    Post->Settings.BloomIntensity = .42f;
    Post->Settings.bOverride_VignetteIntensity = true;
    Post->Settings.VignetteIntensity = .22f;
    Post->Settings.bOverride_MotionBlurAmount = true;
    Post->Settings.MotionBlurAmount = 0.f;
    Post->Settings.bOverride_ColorSaturation = true;
    Post->Settings.ColorSaturation = FVector4(1.08f, 1.08f, 1.08f, 1.f);
    Part(this, TEXT("DistantMoon"), TEXT("Sphere"), FVector(9000.f, 16000.f, 11000.f), FVector(18.f), FLinearColor(.33f, .48f, .8f), true);
}

void ARiftWorld::BuildForest()
{
    FRandomStream Random(4829);
    Part(this, TEXT("ArenaGround"), TEXT("Cube"), FVector(0.f, 0.f, -70.f), FVector(110.f, 110.f, 1.4f), FLinearColor(.105f, .16f, .11f), false, true);
    auto* Moss = Instances(this, TEXT("MossIslands"), TEXT("Cylinder"), FLinearColor(.13f, .21f, .135f));
    auto* Earth = Instances(this, TEXT("EarthIslands"), TEXT("Cylinder"), FLinearColor(.18f, .155f, .115f));
    for (int32 Index = 0; Index < 90; ++Index)
    {
        const FVector Location(Random.FRandRange(-4900.f, 4900.f), Random.FRandRange(-4900.f, 4900.f), 1.3f);
        Instance(Index % 3 ? Moss : Earth, Location, FVector(Random.FRandRange(2.f, 9.f), Random.FRandRange(1.5f, 5.f), .015f), FRotator(0.f, Random.FRandRange(0.f, 360.f), 0.f));
    }
    // A readable trail leads straight from the player's camp toward the hovering ship.
    for (int32 Index = 0; Index < 18; ++Index)
    {
        Instance(Earth, FVector(-2350.f + Index * 250.f, FMath::Sin(Index * .43f) * 80.f, 2.1f), FVector(3.65f, 2.25f, .014f));
    }
    auto* Trunks = Instances(this, TEXT("PineTrunks"), TEXT("Cylinder"), FLinearColor(.13f, .085f, .055f), true);
    auto* FoliageDark = Instances(this, TEXT("PineDeepGreen"), TEXT("Cone"), FLinearColor(.055f, .15f, .125f));
    auto* FoliageLight = Instances(this, TEXT("PineSage"), TEXT("Cone"), FLinearColor(.12f, .255f, .18f));
    for (int32 Index = 0; Index < 500; ++Index)
    {
        FVector Base(Random.FRandRange(-5300.f, 5300.f), Random.FRandRange(-5300.f, 5300.f), 0.f);
        // Keep the center and the full enemy spawn ring navigable without a navmesh.
        if (Base.SizeSquared2D() < FMath::Square(3380.f)) continue;
        const float Height = Random.FRandRange(650.f, 1300.f);
        const float Width = Height * Random.FRandRange(.32f, .42f);
        const float Yaw = Random.FRandRange(0.f, 360.f);
        Instance(Trunks, Base + FVector(0.f, 0.f, Height * .25f), FVector(.34f, .34f, Height * .005f));
        for (int32 Tier = 0; Tier < 3; ++Tier)
        {
            const float Factor = 1.f - Tier * .22f;
            Instance(Index % 3 ? FoliageDark : FoliageLight,
                Base + FVector(0.f, 0.f, Height * (.40f + Tier * .19f)),
                FVector(Width * .01f * Factor, Width * .01f * Factor, Height * .0056f), FRotator(0.f, Yaw, 0.f));
        }
    }
    auto* Pebbles = Instances(this, TEXT("FieldStones"), TEXT("Sphere"), FLinearColor(.22f, .28f, .29f));
    auto* Grass = Instances(this, TEXT("GrassTufts"), TEXT("Cone"), FLinearColor(.24f, .33f, .19f));
    for (int32 Index = 0; Index < 420; ++Index)
    {
        const float X = Random.FRandRange(-5100.f, 5100.f);
        const float Y = Random.FRandRange(-5100.f, 5100.f);
        if (FMath::Abs(Y) < 230.f && X > -2600.f && X < 2500.f) continue;
        const float Size = Random.FRandRange(.16f, .65f);
        Instance(Pebbles, FVector(X, Y, Size * 17.f), FVector(Size, Size * .7f, Size * .45f), FRotator(0.f, Random.FRandRange(0.f, 360.f), 0.f));
        Instance(Grass, FVector(X + 55.f, Y, 18.f), FVector(.3f, .13f, Random.FRandRange(.3f, .8f)), FRotator(0.f, Random.FRandRange(0.f, 360.f), 0.f));
    }
    // Natural rock boundary backs up an invisible solid rim, keeping every player inside the arena.
    auto* BoundaryRocks = Instances(this, TEXT("BoundaryRocks"), TEXT("Sphere"), FLinearColor(.15f, .20f, .23f), true);
    auto* Mountains = Instances(this, TEXT("DistantMountains"), TEXT("Cone"), FLinearColor(.085f, .13f, .19f));
    for (int32 Index = 0; Index < 64; ++Index)
    {
        const float Angle = Index * UE_TWO_PI / 64.f;
        FVector Direction(FMath::Cos(Angle), FMath::Sin(Angle), 0.f);
        const float SquareRadius = 5420.f / FMath::Max(FMath::Abs(Direction.X), FMath::Abs(Direction.Y));
        Instance(BoundaryRocks, Direction * SquareRadius + FVector(0.f, 0.f, 110.f), FVector(Random.FRandRange(4.5f, 8.f), Random.FRandRange(4.5f, 8.f), Random.FRandRange(3.f, 6.f)));
        if (Index % 3 == 0) Instance(Mountains, Direction * Random.FRandRange(8500.f, 13000.f) + FVector(0.f, 0.f, 700.f), FVector(Random.FRandRange(20.f, 38.f), Random.FRandRange(20.f, 38.f), Random.FRandRange(30.f, 55.f)));
    }
    for (int32 Side = 0; Side < 4; ++Side)
    {
        const bool bXSide = Side < 2;
        const float Sign = Side % 2 == 0 ? -1.f : 1.f;
        UStaticMeshComponent* Barrier = Part(this, *FString::Printf(TEXT("ArenaLimit%d"), Side), TEXT("Cube"),
            bXSide ? FVector(Sign * 5500.f, 0.f, 1800.f) : FVector(0.f, Sign * 5500.f, 1800.f),
            bXSide ? FVector(1.f, 112.f, 38.f) : FVector(112.f, 1.f, 38.f), FLinearColor::Black, false, true);
        Barrier->SetVisibility(false);
        Barrier->SetHiddenInGame(true);
    }
    // A broken stone circle visually frames the ship and its later portal.
    for (int32 Index = 0; Index < 12; ++Index)
    {
        const float Angle = Index * UE_TWO_PI / 12.f;
        const FVector Location(1800.f + 700.f * FMath::Cos(Angle), 700.f * FMath::Sin(Angle), 55.f);
        Part(this, *FString::Printf(TEXT("AnomalyStone%d"), Index), TEXT("Cube"), Location,
            FVector(.95f, 1.05f, 1.1f + (Index % 3) * .3f), FLinearColor(.18f, .235f, .25f), false, true)->SetRelativeRotation(FRotator(0.f, FMath::RadiansToDegrees(Angle), 8.f));
        Part(this, *FString::Printf(TEXT("AnomalyRune%d"), Index), TEXT("Cube"), Location + FVector(0.f, 0.f, 65.f + (Index % 3) * 15.f),
            FVector(.55f, .13f, .05f), FLinearColor(.05f, .75f, .8f), true);
    }
}

void ARiftWorld::SpawnCover(const FVector& Location, const FVector& Size, const FLinearColor& Tint, float Yaw)
{
    if (!HasAuthority()) return;
    const FTransform Transform(FRotator(0.f, Yaw, 0.f), Location);
    ARiftProp* Prop = GetWorld()->SpawnActorDeferred<ARiftProp>(ARiftProp::StaticClass(), Transform, this, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
    if (Prop)
    {
        Prop->Size = Size;
        Prop->Tint = Tint;
        UGameplayStatics::FinishSpawningActor(Prop, Transform);
    }
}

void ARiftWorld::BuildCamp()
{
    const FVector Cabin(-500.f, -1700.f, 0.f);
    const FLinearColor Wood(.27f, .13f, .06f);
    Part(this, TEXT("CabinFoundation"), TEXT("Cube"), Cabin + FVector(0.f, 0.f, 12.f), FVector(9.8f, 7.8f, .24f), FLinearColor(.22f, .245f, .24f), false, true);
    // Five independent panels per side; a wide front doorway remains open from the start.
    for (int32 Index = 0; Index < 5; ++Index)
    {
        const float X = -360.f + Index * 180.f;
        SpawnCover(Cabin + FVector(X, -355.f, 177.f), FVector(176.f, 22.f, 306.f), Wood);
        if (Index != 2) SpawnCover(Cabin + FVector(X, 355.f, 177.f), FVector(176.f, 22.f, 306.f), Wood);
    }
    for (int32 Index = 0; Index < 4; ++Index)
    {
        const float Y = -270.f + Index * 180.f;
        SpawnCover(Cabin + FVector(-450.f, Y, 177.f), FVector(22.f, 176.f, 306.f), Wood);
        SpawnCover(Cabin + FVector(450.f, Y, 177.f), FVector(22.f, 176.f, 306.f), Wood);
    }
    const FLinearColor Roof(.13f, .215f, .20f);
    Part(this, TEXT("CabinRoofFront"), TEXT("Cube"), Cabin + FVector(0.f, 210.f, 408.f), FVector(10.5f, 4.8f, .28f), Roof, false, true)->SetRelativeRotation(FRotator(0.f, 0.f, -21.f));
    Part(this, TEXT("CabinRoofBack"), TEXT("Cube"), Cabin + FVector(0.f, -210.f, 408.f), FVector(10.5f, 4.8f, .28f), Roof, false, true)->SetRelativeRotation(FRotator(0.f, 0.f, 21.f));
    Part(this, TEXT("RoofRidge"), TEXT("Cylinder"), Cabin + FVector(0.f, 0.f, 486.f), FVector(.38f, .38f, 10.7f), FLinearColor(.07f, .105f, .095f))->SetRelativeRotation(FRotator(90.f, 0.f, 0.f));
    Part(this, TEXT("CabinDoorHeader"), TEXT("Cube"), Cabin + FVector(0.f, 365.f, 300.f), FVector(1.65f, .3f, .32f), FLinearColor(.42f, .26f, .13f), false, true);
    Part(this, TEXT("CabinDoorStep"), TEXT("Cube"), Cabin + FVector(0.f, 435.f, 8.f), FVector(2.f, 1.1f, .16f), FLinearColor(.27f, .27f, .245f), false, true);
    Part(this, TEXT("CabinChimney"), TEXT("Cube"), Cabin + FVector(-250.f, -120.f, 505.f), FVector(1.f, 1.f, 2.8f), FLinearColor(.19f, .21f, .23f), false, true);
    Part(this, TEXT("CabinTable"), TEXT("Cube"), Cabin + FVector(170.f, -160.f, 90.f), FVector(2.5f, 1.2f, .18f), FLinearColor(.36f, .21f, .1f), false, true);
    for (int32 Side = 0; Side < 2; ++Side)
    {
        Part(this, *FString::Printf(TEXT("TableLeg%d"), Side), TEXT("Cube"), Cabin + FVector(80.f + Side * 180.f, -160.f, 52.f), FVector(.14f, 1.f, .8f), Wood);
    }
    Lamp(this, TEXT("CabinWarmth"), Cabin + FVector(0.f, 0.f, 250.f), FLinearColor(1.f, .42f, .14f), 2800.f, 850.f);
    // Amber lanterns punctuate the trail and let players read the cover silhouettes.
    const FVector Lanterns[] = { FVector(-2000.f,-420.f,0.f), FVector(-1600.f,550.f,0.f), FVector(-350.f,-1090.f,0.f), FVector(200.f,1050.f,0.f), FVector(950.f,-500.f,0.f) };
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(Lanterns); ++Index)
    {
        const FVector Base = Lanterns[Index];
        Part(this, *FString::Printf(TEXT("LanternPost%d"), Index), TEXT("Cylinder"), Base + FVector(0.f, 0.f, 120.f), FVector(.12f,.12f,2.4f), FLinearColor(.16f,.115f,.07f), false, true);
        Part(this, *FString::Printf(TEXT("LanternGlow%d"), Index), TEXT("Cube"), Base + FVector(0.f, 0.f, 255.f), FVector(.27f,.27f,.4f), FLinearColor(1.f,.48f,.13f), true);
        Part(this, *FString::Printf(TEXT("LanternCap%d"), Index), TEXT("Cone"), Base + FVector(0.f,0.f,284.f), FVector(.58f,.58f,.25f), FLinearColor(.08f,.11f,.11f));
        Lamp(this, *FString::Printf(TEXT("LanternLight%d"), Index), Base + FVector(0.f,0.f,245.f), FLinearColor(1.f,.53f,.19f), 1400.f, 650.f);
    }
    // Supply crates are real cover; panels and crates share the same damage/replication path.
    SpawnCover(FVector(-1150.f,480.f,70.f), FVector(145.f,145.f,140.f), FLinearColor(.28f,.32f,.19f), 12.f);
    SpawnCover(FVector(-990.f,500.f,55.f), FVector(110.f,120.f,110.f), FLinearColor(.31f,.24f,.12f), -8.f);
    SpawnCover(FVector(-1090.f,500.f,187.f), FVector(90.f,90.f,94.f), FLinearColor(.23f,.285f,.16f), 5.f);
    SpawnCover(FVector(130.f,900.f,80.f), FVector(180.f,100.f,160.f), FLinearColor(.25f,.29f,.20f), -20.f);
    SpawnCover(FVector(410.f,-690.f,66.f), FVector(200.f,110.f,132.f), FLinearColor(.34f,.22f,.12f), 18.f);
    SpawnCover(Cabin+FVector(-250.f,160.f,90.f), FVector(140.f,100.f,132.f), FLinearColor(.22f,.30f,.20f));
    // A small glowing campfire sits outside the cabin doorway.
    const FVector Fire(-1000.f,-930.f,0.f);
    for (int32 Index=0; Index<9; ++Index)
    {
        float Angle=Index*UE_TWO_PI/9.f;
        Part(this,*FString::Printf(TEXT("FireStone%d"),Index),TEXT("Sphere"),Fire+FVector(70.f*FMath::Cos(Angle),70.f*FMath::Sin(Angle),14.f),FVector(.35f,.28f,.25f),FLinearColor(.25f,.29f,.30f));
    }
    Part(this,TEXT("FireLogA"),TEXT("Cylinder"),Fire+FVector(0.f,0.f,20.f),FVector(.23f,.23f,1.15f),Wood)->SetRelativeRotation(FRotator(82.f,35.f,0.f));
    Part(this,TEXT("FireLogB"),TEXT("Cylinder"),Fire+FVector(0.f,0.f,25.f),FVector(.23f,.23f,1.15f),Wood)->SetRelativeRotation(FRotator(82.f,-35.f,0.f));
    Part(this,TEXT("FireAmber"),TEXT("Cone"),Fire+FVector(0.f,0.f,65.f),FVector(.72f,.72f,1.f),FLinearColor(1.f,.20f,.025f),true);
    Part(this,TEXT("FireCore"),TEXT("Cone"),Fire+FVector(0.f,0.f,58.f),FVector(.37f,.37f,.72f),FLinearColor(1.f,.67f,.12f),true);
    Lamp(this,TEXT("CampfireLight"),Fire+FVector(0.f,0.f,150.f),FLinearColor(1.f,.36f,.08f),2200.f,700.f);
}

ARiftProp::ARiftProp()
{
    bReplicates = true;
    bAlwaysRelevant = true;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("CoverRoot")));
}

void ARiftProp::BeginPlay()
{
    Super::BeginPlay();
    Solid = Part(this, TEXT("CoverSolid"), TEXT("Cube"), FVector::ZeroVector, Size / 100.f, Tint, false, true);
    // The raised ribs give the otherwise simple geometry a readable crate/plank silhouette.
    const bool bPanel = FMath::Min(Size.X,Size.Y) < 45.f;
    if (bPanel)
    {
        const bool bThinY = Size.Y < Size.X;
        for (int32 Index=0; Index<7; ++Index)
        {
            FVector Scale = Size / 100.f;
            Scale.Z = .045f;
            if (bThinY) Scale.Y += .025f; else Scale.X += .025f;
            Details.Add(Part(this,*FString::Printf(TEXT("PlankSeam%d"),Index),TEXT("Cube"),FVector(0.f,0.f,-Size.Z*.42f+Index*Size.Z*.14f),Scale,Tint*.54f));
        }
    }
    else
    {
        for (int32 Side=-1; Side<=1; Side+=2)
        {
            Details.Add(Part(this,*FString::Printf(TEXT("CrateBand%d"),Side+1),TEXT("Cube"),FVector(Side*Size.X*.32f,0.f,0.f),FVector(.11f,Size.Y*.0104f,Size.Z*.0104f),FLinearColor(.09f,.13f,.135f)));
        }
    }
    ApplyState();
}

void ARiftProp::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ARiftProp,bBroken);
    DOREPLIFETIME(ARiftProp,Size);
    DOREPLIFETIME(ARiftProp,Tint);
    DOREPLIFETIME(ARiftProp,Health);
}

float ARiftProp::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
    if (!HasAuthority() || bBroken || DamageAmount <= 0.f) return 0.f;
    const float Applied = FMath::Min(Health,DamageAmount);
    Health = FMath::Max(0.f,Health-DamageAmount);
    if (Health <= 0.f) BreakApart();
    ForceNetUpdate();
    return Applied;
}

void ARiftProp::BreakApart()
{
    if (!HasAuthority() || bBroken) return;
    bBroken = true;
    Health = 0.f;
    ApplyState();
    ForceNetUpdate();
}

void ARiftProp::OnRep_VisualState() { ApplyState(); }

void ARiftProp::ResetCover()
{
    if (!HasAuthority()) return;
    Health = 90.f;
    bBroken = false;
    ApplyState();
    ForceNetUpdate();
}

void ARiftProp::ApplyState()
{
    if (!Solid) return;
    Solid->SetRelativeScale3D(Size/100.f);
    Solid->SetMaterial(0,RiftVisual::Material(this,Tint,false));
    Solid->SetVisibility(!bBroken);
    Solid->SetCollisionEnabled(bBroken ? ECollisionEnabled::NoCollision : ECollisionEnabled::QueryAndPhysics);
    for (UStaticMeshComponent* Detail : Details) Detail->SetVisibility(!bBroken);
    for (UStaticMeshComponent* Piece : Rubble) Piece->SetVisibility(bBroken);
    if (!bBroken || bRubbleBuilt) return;
    bRubbleBuilt = true;
    FRandomStream Random(FMath::RoundToInt(GetActorLocation().X*17.f+GetActorLocation().Y*31.f));
    for (int32 Index=0; Index<9; ++Index)
    {
        const float Length=Random.FRandRange(.5f,1.6f);
        UStaticMeshComponent* Piece=Part(this,*FString::Printf(TEXT("BrokenTimber%d"),Index),TEXT("Cube"),
            FVector(Random.FRandRange(-Size.X*.65f-40.f,Size.X*.65f+40.f),Random.FRandRange(-Size.Y*.65f-40.f,Size.Y*.65f+40.f),-GetActorLocation().Z+8.f+Index*2.f),
            FVector(Length,.16f,Random.FRandRange(.08f,.17f)),Tint*Random.FRandRange(.6f,1.1f));
        Piece->SetRelativeRotation(FRotator(Random.FRandRange(-8.f,8.f),Random.FRandRange(0.f,360.f),Random.FRandRange(-8.f,8.f)));
        Rubble.Add(Piece);
    }
}

ARiftUFO::ARiftUFO()
{
    bReplicates = true;
    bAlwaysRelevant = true;
    PrimaryActorTick.bCanEverTick = true;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("UFORoot")));
}

void ARiftUFO::BeginPlay()
{
    Super::BeginPlay();
    Hull.Add(Part(this,TEXT("ShipSaucer"),TEXT("Sphere"),FVector::ZeroVector,FVector(10.5f,10.5f,1.6f),FLinearColor(.18f,.22f,.28f),false,true));
    Hull.Add(Part(this,TEXT("ShipEdge"),TEXT("Cylinder"),FVector(0.f,0.f,-8.f),FVector(10.7f,10.7f,.18f),FLinearColor(.07f,.65f,.75f),true,true));
    Hull.Add(Part(this,TEXT("ShipUpper"),TEXT("Sphere"),FVector(0.f,0.f,65.f),FVector(4.9f,4.9f,2.6f),FLinearColor(.08f,.18f,.25f),false,true));
    Hull.Add(Part(this,TEXT("ShipCockpit"),TEXT("Sphere"),FVector(0.f,0.f,115.f),FVector(3.7f,3.7f,1.9f),FLinearColor(.05f,.4f,.51f),true,true));
    Hull.Add(Part(this,TEXT("ShipReactor"),TEXT("Sphere"),FVector(0.f,0.f,-95.f),FVector(3.3f,3.3f,.5f),FLinearColor(.08f,.9f,1.f),true));
    for (int32 Index=0; Index<16; ++Index)
    {
        const float Angle=Index*UE_TWO_PI/16.f;
        Hull.Add(Part(this,*FString::Printf(TEXT("ShipRunningLight%d"),Index),TEXT("Sphere"),FVector(470.f*FMath::Cos(Angle),470.f*FMath::Sin(Angle),-38.f),FVector(.32f,.32f,.20f),FLinearColor(.2f,.85f,1.f),true));
    }
    // Ring plane faces the player's approach along +X. It has no collision once opened.
    for (int32 Index=0; Index<40; ++Index)
    {
        const float Angle=Index*UE_TWO_PI/40.f;
        UStaticMeshComponent* Segment=Part(this,*FString::Printf(TEXT("PortalSegment%d"),Index),TEXT("Cube"),FVector(0.f,640.f*FMath::Cos(Angle),640.f*FMath::Sin(Angle)),FVector(.6f,.62f,1.17f),Index%3==0?FLinearColor(.8f,.14f,1.f):FLinearColor(.035f,.8f,1.f),true);
        Segment->SetRelativeRotation(FRotator(0.f,0.f,FMath::RadiansToDegrees(Angle)));
        Portal.Add(Segment);
    }
    for (int32 Index=0; Index<12; ++Index)
    {
        const float Angle=Index*UE_TWO_PI/12.f;
        UStaticMeshComponent* Shard=Part(this,*FString::Printf(TEXT("PortalShard%d"),Index),TEXT("Cube"),FVector(0.f,790.f*FMath::Cos(Angle),790.f*FMath::Sin(Angle)),FVector(.8f,.22f,.8f),FLinearColor(.32f,.08f,.6f),true);
        Portal.Add(Shard);
    }
    GlowLight=Lamp(this,TEXT("AnomalyLight"),FVector(0.f,0.f,-500.f),FLinearColor(.05f,.7f,1.f),5500.f,1900.f);
    ApplyState();
}

void ARiftUFO::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ARiftUFO,bActivated);
    DOREPLIFETIME(ARiftUFO,Health);
}

float ARiftUFO::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
    if (!HasAuthority() || bActivated || DamageAmount<=0.f) return 0.f;
    const float Applied=FMath::Min(DamageAmount,Health);
    Health=FMath::Max(0.f,Health-DamageAmount);
    if (Health<=0.f)
    {
        bActivated=true;
        ApplyState();
        if (ARiftGameMode* Mode=GetWorld()->GetAuthGameMode<ARiftGameMode>()) Mode->TriggerAnomaly();
    }
    ForceNetUpdate();
    return Applied;
}

void ARiftUFO::OnRep_Activated() { ApplyState(); }

void ARiftUFO::ResetAnomaly()
{
    if (!HasAuthority()) return;
    Health = 180.f;
    bActivated = false;
    ApplyState();
    ForceNetUpdate();
}

void ARiftUFO::ApplyState()
{
    for (int32 Index = 0; Index < Hull.Num(); ++Index)
    {
        UStaticMeshComponent* Component = Hull[Index];
        Component->SetVisibility(!bActivated);
        Component->SetCollisionEnabled(!bActivated && Index < 4 ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
    }
    for (UStaticMeshComponent* Component:Portal) Component->SetVisibility(bActivated);
    if (GlowLight)
    {
        GlowLight->SetLightColor(bActivated?FLinearColor(.4f,.08f,1.f):FLinearColor(.05f,.7f,1.f));
        GlowLight->SetIntensity(bActivated?9000.f:5500.f);
    }
}

void ARiftUFO::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    AnimationTime+=DeltaSeconds;
    if (!bActivated)
    {
        // Animate components only: the authoritative actor transform stays stable for hit registration.
        for (int32 Index=5; Index<Hull.Num(); ++Index)
        {
            const float Angle=(Index-5)*UE_TWO_PI/16.f+AnimationTime*.25f;
            Hull[Index]->SetRelativeLocation(FVector(470.f*FMath::Cos(Angle),470.f*FMath::Sin(Angle),-38.f));
        }
        return;
    }
    for (int32 Index=0; Index<Portal.Num(); ++Index)
    {
        const bool bOuter=Index>=40;
        const float Angle=(bOuter?(Index-40)*UE_TWO_PI/12.f:Index*UE_TWO_PI/40.f)+AnimationTime*(bOuter?-.26f:.18f);
        const float Radius=bOuter?790.f+20.f*FMath::Sin(AnimationTime*2.f+Index):640.f;
        Portal[Index]->SetRelativeLocation(FVector(bOuter?FMath::Sin(AnimationTime+Index)*55.f:0.f,Radius*FMath::Cos(Angle),Radius*FMath::Sin(Angle)));
        Portal[Index]->SetRelativeRotation(FRotator(0.f,0.f,FMath::RadiansToDegrees(Angle)+(bOuter?45.f:0.f)));
    }
}
