#include "VoyagerCosmos.h"
#include "VoyagerData.h"
#include "VoyagerGameMode.h"
#include "VoyagerShip.h"
#include "VoyagerCharacter.h"
#include "RiftVisual.h"
#include "ProceduralMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"

namespace VoyagerCosmos
{
    FVector AnomalyCenter(int32 System)
    {
        const uint32 Seed=Voyager::Hash(uint32(System)*8191u+713u);
        // A distant encounter outside all five planetary orbits; no singularity
        // is inserted into the city, atmosphere or the ordinary cruise lanes.
        const double Side=(Seed&1)?1.0:-1.0;
        return FVector(Side*(12400.0+Seed%1800),9800.0+(Seed>>8)%2200,4200.0+(Seed>>16)%1700)*Voyager::CentimetersPerKm;
    }
    double HorizonRadius(int32 System)
    {
        return (70.0+Voyager::Hash(uint32(System)+171u)%41)*Voyager::CentimetersPerKm;
    }
    FVector SurveyPoint(int32 System)
    {
        return AnomalyCenter(System)+FVector(-.78,-.58,.24).GetSafeNormal()*HorizonRadius(System)*18.0;
    }
    float TidalDamagePerSecond(int32 System,const FVector& Position)
    {
        const double R=FVector::Distance(Position,AnomalyCenter(System))/HorizonRadius(System);
        // Bounded gameplay tides leave a broad, safe observation orbit. This is
        // deliberately not a claim to model real event-horizon physics.
        if(R>=5.0)return 0.f;
        return float(FMath::Clamp((5.0-R)*(5.0-R)*3.0,0.0,80.0));
    }
}

namespace
{
    UProceduralMeshComponent* Annulus(AActor* Owner,FName Name,double Inner,double Outer,
        int32 Bands,UMaterialInterface* Material,bool bHalf=false)
    {
        auto* Mesh=NewObject<UProceduralMeshComponent>(Owner,Name);
        Owner->AddInstanceComponent(Mesh);Mesh->SetupAttachment(Owner->GetRootComponent());
        Mesh->SetMobility(EComponentMobility::Movable);Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Mesh->SetCastShadow(false);Mesh->SetCanEverAffectNavigation(false);Mesh->SetMaterial(0,Material);Mesh->RegisterComponent();
        TArray<FVector> V,N;TArray<int32> Tri;TArray<FVector2D> UV;TArray<FLinearColor> Color;TArray<FProcMeshTangent> Tangents;
        constexpr int32 Segments=256;
        for(int32 R=0;R<=Bands;++R)for(int32 A=0;A<=Segments;++A)
        {
            const double U=double(A)/Segments,T=double(R)/Bands,Theta=U*UE_DOUBLE_PI*(bHalf?1.0:2.0);
            const double Radius=FMath::Lerp(Inner,Outer,T);
            V.Add(FVector(FMath::Cos(Theta)*Radius,FMath::Sin(Theta)*Radius,0));
            N.Add(FVector::UpVector);UV.Add(FVector2D(U,T));Color.Add(FLinearColor::White);
        }
        for(int32 R=0;R<Bands;++R)for(int32 A=0;A<Segments;++A)
        {
            const int32 I=R*(Segments+1)+A,J=I+Segments+1;
            Tri.Append({I,J,I+1,I+1,J,J+1});
        }
        Mesh->CreateMeshSection_LinearColor(0,V,Tri,N,UV,Color,Tangents,false);return Mesh;
    }
}

AVoyagerCosmos::AVoyagerCosmos()
{
    bReplicates=false;PrimaryActorTick.bCanEverTick=true;PrimaryActorTick.TickInterval=.04f;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("CosmosRoot")));
}
AVoyagerCosmos* AVoyagerCosmos::Find(UWorld* World)
{
    if(World)for(TActorIterator<AVoyagerCosmos> It(World);It;++It)return *It;
    return nullptr;
}
void AVoyagerCosmos::Build(int32 System)
{
    for(UActorComponent* Part:{static_cast<UActorComponent*>(Horizon.Get()),static_cast<UActorComponent*>(Disk.Get()),
        static_cast<UActorComponent*>(PhotonRing.Get()),static_cast<UActorComponent*>(LensedDisk.Get())})
        if(Part){RemoveInstanceComponent(Part);Part->DestroyComponent();}
    Horizon=nullptr;Disk=nullptr;PhotonRing=nullptr;LensedDisk=nullptr;WarningTimes.Empty();
    BuiltSystem=System;SetActorLocation(VoyagerCosmos::AnomalyCenter(System));
    if(GetNetMode()==NM_DedicatedServer)return;
    auto* DiskBase=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Cosmos/M_CosmosDisk.M_CosmosDisk"));
    auto* DarkBase=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Cosmos/M_CosmosHorizon.M_CosmosHorizon"));
    if(!DiskBase||!DarkBase){UE_LOG(LogTemp,Error,TEXT("VOYAGER COSMOS MISSING MATERIALS"));return;}
    const double R=VoyagerCosmos::HorizonRadius(System);
    Horizon=RiftVisual::Mesh(this,GetRootComponent(),MakeUniqueObjectName(this,UStaticMeshComponent::StaticClass(),TEXT("EventHorizon")),
        TEXT("Sphere"),FVector::ZeroVector,FVector(R/50.0),FLinearColor::Black);
    AddInstanceComponent(Horizon);Horizon->SetMaterial(0,DarkBase);Horizon->SetCastShadow(false);
    auto* Material=UMaterialInstanceDynamic::Create(DiskBase,this);
    const float BlueFraction=float(Voyager::Hash(uint32(System))%100)/400.f;
    Material->SetVectorParameterValue(TEXT("OuterTint"),FMath::Lerp(FLinearColor(1.f,.21f,.025f),FLinearColor(.1f,.4f,1.f),BlueFraction));
    Material->SetScalarParameterValue(TEXT("Seed"),float(System%997));
    Disk=Annulus(this,MakeUniqueObjectName(this,UProceduralMeshComponent::StaticClass(),TEXT("AccretionDisk")),R*1.65,R*6.0,24,Material);
    Disk->SetRelativeRotation(FRotator(12.f,23.f,float(System%35)-17.f));
    auto* RingMaterial=UMaterialInstanceDynamic::Create(DiskBase,this);
    RingMaterial->SetScalarParameterValue(TEXT("RingMode"),1.f);
    RingMaterial->SetVectorParameterValue(TEXT("OuterTint"),FLinearColor(.7f,.83f,1.f));
    RingMaterial->SetScalarParameterValue(TEXT("Intensity"),.85f);
    PhotonRing=Annulus(this,MakeUniqueObjectName(this,UProceduralMeshComponent::StaticClass(),TEXT("PhotonRing")),R*1.012,R*1.085,4,RingMaterial);
    auto* LensMaterial=UMaterialInstanceDynamic::Create(DiskBase,this);
    LensMaterial->SetScalarParameterValue(TEXT("RingMode"),.75f);
    LensMaterial->SetScalarParameterValue(TEXT("Intensity"),1.2f);
    LensMaterial->SetVectorParameterValue(TEXT("OuterTint"),FLinearColor(.2f,.48f,1.f));
    LensedDisk=Annulus(this,MakeUniqueObjectName(this,UProceduralMeshComponent::StaticClass(),TEXT("LensedFarDisk")),R*1.15,R*1.5,8,LensMaterial,true);
    UE_LOG(LogTemp,Display,TEXT("VOYAGER COSMOS READY system=%d center=%s horizon_km=%.1f survey=%s"),System,*GetActorLocation().ToCompactString(),R/Voyager::CentimetersPerKm,*VoyagerCosmos::SurveyPoint(System).ToCompactString());
}
void AVoyagerCosmos::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    const auto* State=GetWorld()->GetGameState<AVoyagerState>();if(!State)return;
    if(BuiltSystem!=State->SystemSeed)Build(State->SystemSeed);
    if(PhotonRing&&LensedDisk)
    {
        if(const auto* PC=GetWorld()->GetFirstPlayerController())
        {
            FVector Eye;FRotator View;PC->GetPlayerViewPoint(Eye,View);
            const FVector Facing=(Eye-GetActorLocation()).GetSafeNormal();
            const FRotator Rotation=FRotationMatrix::MakeFromZY(Facing,FVector::UpVector).Rotator();
            PhotonRing->SetWorldRotation(Rotation);LensedDisk->SetWorldRotation(Rotation);
            // These are analytic presentation arcs. They do not bend scene rays.
            LensedDisk->SetRelativeScale3D(FVector(1.12,1.04,1));
        }
    }
    if(GetNetMode()==NM_Client)return;
    HazardCountdown-=DeltaSeconds;if(HazardCountdown>0)return;HazardCountdown=.5f;
    const double Now=GetWorld()->GetTimeSeconds(),R=VoyagerCosmos::HorizonRadius(BuiltSystem);
    for(auto It=WarningTimes.CreateIterator();It;++It)if(!It.Key().IsValid())It.RemoveCurrent();
    for(TActorIterator<AVoyagerShip> It(GetWorld());It;++It)
    {
        auto* Ship=*It;if(!Ship->IsPlayerControlled()||Ship->bLanded)continue;
        const double Distance=FVector::Distance(Ship->GetActorLocation(),GetActorLocation());
        if(Distance<R*8.0)
        {
            const double* Previous=WarningTimes.Find(Ship);
            if(!Previous||Now-*Previous>7.0)
            {
                if(auto* PC=Cast<AVoyagerController>(Ship->GetController()))PC->Notify(TEXT("SINGULARITY WARNING: extreme tidal forces ahead. Turn away from the dark horizon."));
                WarningTimes.Add(Ship,Now);
            }
        }
        const float Damage=VoyagerCosmos::TidalDamagePerSecond(BuiltSystem,Ship->GetActorLocation())*.5f;
        if(Damage>0.f)UGameplayStatics::ApplyDamage(Ship,Damage,nullptr,this,nullptr);
    }
}
