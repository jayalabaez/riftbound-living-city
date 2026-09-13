#include "VoyagerShipVisuals.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"

namespace VoyagerShipVisuals
{
    namespace
    {
        UStaticMeshComponent* Part(AActor* Owner, USceneComponent* Parent, const TCHAR* Name,
            const TCHAR* MeshName, const FVector& Location = FVector::ZeroVector)
        {
            UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr,
                *FString::Printf(TEXT("/Game/Ships/Meshes/%s.%s"), MeshName, MeshName));
            if (!Mesh) return nullptr;
            auto* Component = NewObject<UStaticMeshComponent>(Owner, FName(Name));
            Component->SetNetAddressable();
            Component->SetupAttachment(Parent);
            Component->SetMobility(EComponentMobility::Movable);
            Component->SetStaticMesh(Mesh);
            Component->SetRelativeLocation(Location);
            Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Component->SetGenerateOverlapEvents(false);
            Component->SetCanEverAffectNavigation(false);
            Owner->AddInstanceComponent(Component);
            Component->RegisterComponent();
            return Component;
        }

        UMaterialInstanceDynamic* TintSlot(UStaticMeshComponent* Component, int32 Index, const FLinearColor& Tint)
        {
            if (!Component) return nullptr;
            auto* Material = Component->CreateDynamicMaterialInstance(Index);
            if (Material) Material->SetVectorParameterValue(TEXT("Tint"), Tint);
            return Material;
        }

        void Marking(AActor* Owner, USceneComponent* Parent, const TCHAR* Name,
            const TCHAR* Text, const FVector& Position, float Size)
        {
            auto* Label = NewObject<UTextRenderComponent>(Owner, FName(Name));
            Label->SetNetAddressable();
            Label->SetupAttachment(Parent);
            Label->SetRelativeLocation(Position);
            Label->SetRelativeRotation(FRotator(90, 0, 0));
            Label->SetText(FText::FromString(Text));
            Label->SetHorizontalAlignment(EHTA_Center);
            Label->SetVerticalAlignment(EVRTA_TextCenter);
            Label->SetWorldSize(Size);
            Label->SetTextRenderColor(FColor(218, 224, 224));
            Label->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Label->SetCastShadow(false);
            Owner->AddInstanceComponent(Label);
            Label->RegisterComponent();
        }
    }

    FAssembly Build(AActor* Owner, USceneComponent* Parent, ELivery Livery)
    {
        FAssembly Assembly;
        if (!Owner || !Parent || Owner->GetNetMode() == NM_DedicatedServer) return Assembly;
        auto* Hull = Part(Owner, Parent, TEXT("KestrelHull"), TEXT("SM_KestrelHull"));
        if (!Hull)
        {
            UE_LOG(LogTemp, Warning, TEXT("VOYAGER SHIP VISUALS missing imported /Game/Ships/Meshes/SM_KestrelHull"));
            return Assembly;
        }
        Assembly.Hull = Hull;
        Assembly.Canopy = Part(Owner, Parent, TEXT("KestrelCanopy"), TEXT("SM_KestrelCanopy"));
        Assembly.Gear = Part(Owner, Parent, TEXT("KestrelLandingGear"), TEXT("SM_KestrelGear"));
        Assembly.bPatrol = Livery == ELivery::Patrol;
        const FLinearColor Paint = Livery == ELivery::Patrol ? FLinearColor(.62f,.66f,.68f) :
            Livery == ELivery::Raider ? FLinearColor(.095f,.105f,.115f) : FLinearColor(.56f,.60f,.59f);
        const FLinearColor Stripe = Livery == ELivery::Patrol ? FLinearColor(.012f,.027f,.048f) :
            Livery == ELivery::Raider ? FLinearColor(.27f,.046f,.018f) : FLinearColor(.012f,.12f,.20f);
        const FLinearColor Engine = Livery == ELivery::Raider ? FLinearColor(1.f,.24f,.055f) : FLinearColor(.10f,.58f,1.f);
        // Slots are authored in the source manifest and verified by the importer.
        TintSlot(Hull, 0, Paint);
        TintSlot(Hull, 3, Stripe);
        TintSlot(Hull, 6, Engine);
        if (Assembly.Gear.IsValid()) TintSlot(Assembly.Gear.Get(), 2, Paint);
        if (Assembly.Canopy.IsValid())
        {
            Assembly.Canopy->SetCastShadow(false);
            Assembly.Canopy->SetTranslucentSortPriority(1);
        }
        for (int32 Side = -1; Side <= 1; Side += 2)
        {
            auto* Exhaust = Part(Owner, Parent, *FString::Printf(TEXT("KestrelExhaust%d"), Side),
                TEXT("SM_KestrelExhaust"), FVector(-355, Side*179, -5));
            if (Exhaust)
            {
                Exhaust->SetCastShadow(false);
                Assembly.Exhausts.Add(Exhaust);
                Assembly.ExhaustMaterials.Add(TintSlot(Exhaust, 0, Engine));
            }
            if (Assembly.bPatrol)
            {
                auto* Beacon = Part(Owner, Parent, *FString::Printf(TEXT("KestrelPatrolBeacon%d"), Side),
                    TEXT("SM_KestrelBeacon"), FVector(-94, Side*72, 68));
                if (Beacon)
                {
                    Beacon->SetCastShadow(false);
                    Assembly.Beacons.Add(Beacon);
                    Assembly.BeaconMaterials.Add(TintSlot(Beacon, 0,
                        Side < 0 ? FLinearColor(.02f,.16f,1.f) : FLinearColor(1.f,.018f,.008f)));
                }
            }
        }
        Marking(Owner, Parent, TEXT("KestrelHullMarking"), Assembly.bPatrol ? TEXT("PATROL") :
            Livery == ELivery::Raider ? TEXT("R-19") : TEXT("KESTREL  /  07"), FVector(-205,0,70), Assembly.bPatrol ? 23.f : 15.f);
        Update(Assembly, 0.f, 0.f, .15f, true);
        UE_LOG(LogTemp, Display, TEXT("VOYAGER SHIP VISUALS imported=true livery=%d hull_lods=%d canopy=%d gear=%d exhausts=%d"),
            int32(Livery), Hull->GetStaticMesh()->GetNumLODs(), Assembly.Canopy.IsValid(), Assembly.Gear.IsValid(), Assembly.Exhausts.Num());
        return Assembly;
    }

    void Update(FAssembly& Assembly, float DeltaSeconds, float Age, float Thrust, bool bLanded, bool bAlert)
    {
        if (!Assembly.IsReady()) return;
        const float TargetGear = bLanded ? 1.f : 0.f;
        Assembly.GearFraction = FMath::FInterpConstantTo(Assembly.GearFraction, TargetGear, DeltaSeconds, .8f);
        if (auto* Gear = Assembly.Gear.Get())
        {
            Gear->SetVisibility(Assembly.GearFraction > .01f);
            Gear->SetRelativeLocation(FVector(0,0,100.f*(1.f-Assembly.GearFraction)));
        }
        for (int32 Index = 0; Index < Assembly.Exhausts.Num(); ++Index)
        {
            const float Flicker = 1.f + FMath::Sin(Age*29.f + Index*1.4f)*.04f;
            if (auto* Exhaust = Assembly.Exhausts[Index].Get())
            {
                Exhaust->SetVisibility(!bLanded);
                Exhaust->SetRelativeScale3D(FVector(FMath::Clamp(.2f + Thrust*.6f,.25f,1.7f)*Flicker,1,1));
            }
            if (Assembly.ExhaustMaterials.IsValidIndex(Index))
                if (auto* Material = Assembly.ExhaustMaterials[Index].Get())
                    Material->SetScalarParameterValue(TEXT("Glow"), FMath::Clamp(3.f + Thrust*2.f,3.f,8.f)*Flicker);
        }
        for (int32 Index = 0; Index < Assembly.BeaconMaterials.Num(); ++Index)
            if (auto* Material = Assembly.BeaconMaterials[Index].Get())
            {
                const bool bLit = FMath::Fmod(Age*5.f + Index*.5f,1.f) < .38f;
                Material->SetScalarParameterValue(TEXT("Glow"), bAlert ? (bLit ? 9.f : .12f) : .35f);
            }
    }
}
