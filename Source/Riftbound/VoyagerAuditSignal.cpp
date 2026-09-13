#include "VoyagerAuditSignal.h"
#include "Components/SceneComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/CommandLine.h"
#include "Net/UnrealNetwork.h"

namespace
{
bool Enabled(uint8 Kind)
{
    return (Kind==1&&FParse::Param(FCommandLine::Get(),TEXT("VoyagerHuntAudit")))||
        (Kind==2&&FParse::Param(FCommandLine::Get(),TEXT("VoyagerDestructionAudit")))||
        (Kind==3&&FParse::Param(FCommandLine::Get(),TEXT("VoyagerPoliceAudit")));
}
}
AVoyagerAuditSignal::AVoyagerAuditSignal()
{
    bReplicates=true;bOnlyRelevantToOwner=true;SetReplicateMovement(false);SetNetUpdateFrequency(10);
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("AuditSignalRoot")));
}
void AVoyagerAuditSignal::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{Super::GetLifetimeReplicatedProps(OutLifetimeProps);DOREPLIFETIME(AVoyagerAuditSignal,Kind);DOREPLIFETIME(AVoyagerAuditSignal,Subject);DOREPLIFETIME(AVoyagerAuditSignal,Acknowledged);}
void AVoyagerAuditSignal::ServerAcknowledge_Implementation(uint8 Phase)
{
    if(!HasAuthority()||!Enabled(Kind)||!Cast<APlayerController>(GetOwner())||Phase<1||Phase>4||Phase<=Acknowledged)return;
    Acknowledged=Phase;ForceNetUpdate();
}
AVoyagerAuditSignal* AVoyagerAuditSignal::Find(UWorld* World,APlayerController* Controller,uint8 K)
{
    if(!World||!Enabled(K))return nullptr;
    for(TActorIterator<AVoyagerAuditSignal> It(World);It;++It)if(It->Kind==K&&(!Controller||It->GetOwner()==Controller))return *It;
    return nullptr;
}
AVoyagerAuditSignal* AVoyagerAuditSignal::EnsurePeer(UWorld* World,uint8 K)
{
    if(!World||World->GetNetMode()!=NM_ListenServer||!Enabled(K))return nullptr;
    if(auto Existing=Find(World,nullptr,K))return Existing;
    for(FConstPlayerControllerIterator It=World->GetPlayerControllerIterator();It;++It)
    {
        auto PC=It->Get();if(!PC||PC->IsLocalController()||!PC->GetPawn()||PC->AcknowledgedPawn!=PC->GetPawn())continue;
        FActorSpawnParameters Params;Params.Owner=PC;
        auto Signal=World->SpawnActor<AVoyagerAuditSignal>(AVoyagerAuditSignal::StaticClass(),FTransform::Identity,Params);
        if(Signal){Signal->Kind=K;Signal->ForceNetUpdate();}return Signal;
    }
    return nullptr;
}
