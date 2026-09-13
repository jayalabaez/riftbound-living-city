#include "VoyagerCitizen.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Animation/AnimationAsset.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/MaterialInstanceDynamic.h"

namespace VoyagerCitizenVisual
{
    constexpr uint8 TalkActivity=8;
    enum EClip : int32 { Idle,Walk,Run,Talk,Death,Count };
    const TCHAR* ClipNames[]={TEXT("Idle"),TEXT("Walk"),TEXT("Run"),TEXT("Talk"),TEXT("Death")};
}

void AVoyagerCitizen::ClearVisuals()
{
    for(int32 I=Parts.Num()-1;I>=0;--I)
        if(IsValid(Parts[I]))Parts[I]->DestroyComponent();
    Parts.Empty();Materials.Empty();DetailParts.Empty();
    Hips.Empty();Knees.Empty();Shoulders.Empty();Elbows.Empty();
    CharacterAnimations.Empty();CitizenMesh=nullptr;
    BodyRoot=nullptr;HeadPivot=nullptr;bVisualsBuilt=false;
}

void AVoyagerCitizen::BuildVisuals()
{
    if(!Identity.bInitialized||GetNetMode()==NM_DedicatedServer)return;
    ClearVisuals();
    const TCHAR* Variant=Identity.Role==4?TEXT("Guard"):(Identity.Seed%3==0?TEXT("Female"):TEXT("Male"));
    const FString MeshPath=FString::Printf(TEXT("/Game/Characters/Import/%s/Citizen%s/SkeletalMeshes/Citizen%s.Citizen%s"),Variant,Variant,Variant,Variant);
    auto* Mesh=LoadObject<USkeletalMesh>(nullptr,*MeshPath);
    if(!Mesh)
    {
        UE_LOG(LogTemp,Error,TEXT("VOYAGER CHARACTER MISSING %s; run character asset bootstrap"),*MeshPath);
        return;
    }
    VisualRoot->SetWorldScale3D(FVector(Identity.Scale));
    VisualRoot->SetWorldLocationAndRotation(GetActorLocation(),GetActorQuat());
    CitizenMesh=NewObject<USkeletalMeshComponent>(this,TEXT("ClothedCitizen"));
    AddInstanceComponent(CitizenMesh);CitizenMesh->SetupAttachment(VisualRoot);
    CitizenMesh->SetSkeletalMeshAsset(Mesh);
    // glTF +Z forward imports as Unreal +Y; the actor's radial frame uses +X.
    CitizenMesh->SetRelativeRotation(FRotator(0,-90,0));
    CitizenMesh->SetAnimationMode(EAnimationMode::AnimationSingleNode);
    CitizenMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    CitizenMesh->SetGenerateOverlapEvents(false);
    CitizenMesh->SetCanEverAffectNavigation(false);
    CitizenMesh->SetCastShadow(true);
    CitizenMesh->SetCullDistance(48000.f);
    CitizenMesh->bEnableUpdateRateOptimizations=true;
    CitizenMesh->VisibilityBasedAnimTickOption=EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
    CitizenMesh->ComponentTags.Add(TEXT("VoyagerHumanoid"));
    CitizenMesh->RegisterComponent();CitizenMesh->AddTickPrerequisiteActor(this);
    Parts.Add(CitizenMesh);BodyRoot=CitizenMesh;
    FRandomStream Appearance(Identity.Seed);
    static const FLinearColor UniformTints[]={FLinearColor(.91f,1.f,1.03f),FLinearColor(1.05f,.96f,.86f),
        FLinearColor(.94f,.90f,1.04f),FLinearColor(1.04f,1.05f,1.04f),FLinearColor(.56f,.70f,.86f),FLinearColor(.89f,1.f,.87f)};
    for(int32 I=0;I<CitizenMesh->GetNumMaterials();++I)
        if(UMaterialInterface* Base=CitizenMesh->GetMaterial(I))
        {
            auto* Material=UMaterialInstanceDynamic::Create(Base,this);
            const FString Name=Base->GetName();
            FLinearColor Tint=FLinearColor::White;
            if(Name.EndsWith(TEXT("_Cloth")))Tint=UniformTints[FMath::Clamp(Identity.Role,0,5)]*Appearance.FRandRange(.89f,1.08f);
            else if(Name.EndsWith(TEXT("_Hair")))Tint=FLinearColor(Appearance.FRandRange(.65f,1.f),Appearance.FRandRange(.64f,.93f),Appearance.FRandRange(.61f,.88f));
            else if(Name.EndsWith(TEXT("_Skin")))Tint=FLinearColor(Appearance.FRandRange(.95f,1.03f),Appearance.FRandRange(.94f,1.02f),Appearance.FRandRange(.93f,1.02f));
            Material->SetVectorParameterValue(TEXT("Tint"),Tint);
            CitizenMesh->SetMaterial(I,Material);Materials.Add(Material);
        }
    for(int32 I=0;I<VoyagerCitizenVisual::Count;++I)
    {
        const FString Name=FString::Printf(TEXT("Citizen%sA_Citizen%s_%s"),Variant,Variant,VoyagerCitizenVisual::ClipNames[I]);
        const FString Path=FString::Printf(TEXT("/Game/Characters/Import/%s/Citizen%s/SkeletalMeshes/%s.%s"),Variant,Variant,*Name,*Name);
        UAnimationAsset* Clip=LoadObject<UAnimationAsset>(nullptr,*Path);
        CharacterAnimations.Add(Clip);
        if(!Clip)UE_LOG(LogTemp,Error,TEXT("VOYAGER CHARACTER MISSING ANIMATION %s"),*Path);
    }
    if(CharacterAnimations[VoyagerCitizenVisual::Idle])
        CitizenMesh->PlayAnimation(CharacterAnimations[VoyagerCitizenVisual::Idle],true);
    bVisualsBuilt=true;bDetailsVisible=true;
    InteractionCapsule->SetRelativeScale3D(FVector(Identity.Scale));
    Animate(0.f);
}

void AVoyagerCitizen::Animate(float D)
{
    if(!bVisualsBuilt||!CitizenMesh)return;
    const double Time=SynchronizedTime();
    const float Speed=IsAlive()?float(Pose.Velocity.Size()):0.f;
    const float Extrapolation=IsAlive()?FMath::Clamp(float(Time)-Pose.ServerTime,0.f,.12f):0.f;
    const FVector RenderAt=Pose.Location+Pose.Velocity*Extrapolation;
    if(D<=0.f||FVector::DistSquared(VisualRoot->GetComponentLocation(),RenderAt)>FMath::Square(2000.f))
        VisualRoot->SetWorldLocation(RenderAt);
    else VisualRoot->SetWorldLocation(FMath::VInterpTo(VisualRoot->GetComponentLocation(),RenderAt,D,15.f));
    VisualRoot->SetWorldRotation(D<=0.f?Pose.Rotation.Quaternion():
        FQuat::Slerp(VisualRoot->GetComponentQuat(),Pose.Rotation.Quaternion(),1.f-FMath::Exp(-D*13.f)).GetNormalized());
    const int32 Clip=!IsAlive()?VoyagerCitizenVisual::Death:
        Speed>185.f?VoyagerCitizenVisual::Run:Speed>12.f?VoyagerCitizenVisual::Walk:
        Pose.Activity==VoyagerCitizenVisual::TalkActivity?VoyagerCitizenVisual::Talk:VoyagerCitizenVisual::Idle;
    if(!CharacterAnimations.IsValidIndex(Clip)||!CharacterAnimations[Clip])return;
    UAnimSingleNodeInstance* Animation=CitizenMesh->GetSingleNodeInstance();
    if(!Animation)return;
    if(Animation->GetAnimationAsset()!=CharacterAnimations[Clip])
        Animation->SetAnimationAsset(CharacterAnimations[Clip],Clip!=VoyagerCitizenVisual::Death,0.f);
    Animation->SetPlaying(false);
    const float Length=FMath::Max(.01f,Animation->GetLength());
    float Position=0.f;
    if(Clip==VoyagerCitizenVisual::Death)
        Position=FMath::Clamp(float(Time)-DeathTime,0.f,Length);
    else if(Clip==VoyagerCitizenVisual::Walk||Clip==VoyagerCitizenVisual::Run)
    {
        const float Stride=Clip==VoyagerCitizenVisual::Run?210.f:145.f;
        Position=FMath::Fmod((Pose.GaitDistance+Speed*Extrapolation)/Stride,1.f)*Length;
    }
    else Position=float(FMath::Fmod(Time+double(Identity.Ordinal)*.37,double(Length)));
    // Authoritative gait distance and death timestamp keep all clients on the
    // same skinned pose without replicating bones or simulating local ragdolls.
    Animation->SetPosition(Position,false);
}
