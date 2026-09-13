#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/StaticMesh.h"

namespace RiftVisual
{
inline UMaterialInstanceDynamic* Material(UObject* Owner, FLinearColor Color, bool bGlow = false)
{
    UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, bGlow ? TEXT("/Game/Materials/M_Glow.M_Glow") : TEXT("/Game/Materials/M_Surface.M_Surface"));
    if (!Base) Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(Base, Owner);
    if (M) { M->SetVectorParameterValue(TEXT("Tint"), Color); M->SetVectorParameterValue(TEXT("Color"), Color); M->SetScalarParameterValue(TEXT("Glow"), bGlow ? 3.5f : 0.0f); }
    return M;
}
inline UStaticMeshComponent* Mesh(AActor* Owner, USceneComponent* Parent, FName Name, const TCHAR* Shape, FVector Loc, FVector Scale, FLinearColor Color, bool bGlow = false)
{
    UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(Owner, Name);
    C->SetNetAddressable();
    C->SetupAttachment(Parent);
    C->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, *FString::Printf(TEXT("/Engine/BasicShapes/%s.%s"), Shape, Shape)));
    C->SetRelativeLocation(Loc); C->SetRelativeScale3D(Scale);
    C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    C->SetMaterial(0, Material(Owner, Color, bGlow));
    C->SetCastShadow(!bGlow);
    C->RegisterComponent();
    return C;
}
}
