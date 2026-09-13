#include "VoyagerCharacter.h"
#include "VoyagerCitizen.h"
#include "VoyagerSettlement.h"
#include "VoyagerGameMode.h"
#include "VoyagerData.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
    struct FCityNetworkAuditData
    {
        TWeakObjectPtr<AVoyagerController> Remote;
        TWeakObjectPtr<AVoyagerCitizen> Subject;
        TMap<TWeakObjectPtr<AVoyagerCitizen>,FVector> Positions;
        FString Greeting;
        FVector ResumePosition=FVector::ZeroVector;
        int32 InitialSystem=INDEX_NONE;
    };
    // Automation-only state is local to the controller in each real process.
    TMap<TWeakObjectPtr<AVoyagerController>,FCityNetworkAuditData> CityNetworkAudits;
}

void AVoyagerController::RunCityNetworkAudit(float DeltaSeconds)
{
    if(!IsLocalController()||TestStage>=90||TestTime<5.f)return;
    auto* Explorer=Cast<AVoyagerCharacter>(GetPawn());auto* State=GetWorld()->GetGameState<AVoyagerState>();
    if(!Explorer||!State)return;
    auto& Audit=CityNetworkAudits.FindOrAdd(this);const int32 Net=int32(GetNetMode());
    if(Audit.InitialSystem==INDEX_NONE)Audit.InitialSystem=State->SystemSeed;
    auto Fail=[this,Net](const TCHAR* Why)
    {UE_LOG(LogTemp,Error,TEXT("VOYAGER CITY NETWORK AUDIT FAIL %s net=%d stage=%d"),Why,Net,TestStage);TestStage=99;};
    auto Advance=[this](){++TestStage;StageStarted=TestTime;};
    if(State->SystemSeed!=Audit.InitialSystem){Fail(TEXT("UNEXPECTED_SYSTEM_CHANGE"));return;}
    if(TestTime>100.f){Fail(TEXT("TIMEOUT"));return;}
    const float Elapsed=TestTime-StageStarted;
    AVoyagerCitizenManager* Manager=nullptr;
    for(TActorIterator<AVoyagerCitizenManager> It(GetWorld());It;++It){Manager=*It;break;}
    if(!Manager)return;
    if(HasAuthority())
    {
        if(GetNetMode()!=NM_ListenServer){Fail(TEXT("REQUIRES_LISTEN_SERVER"));return;}
        auto Place=[](AVoyagerController* Controller,AVoyagerCharacter* SubjectPawn,FVector Location,FVector Up,FVector Forward)
        {
            SubjectPawn->SetBase(static_cast<FMovementBaseInterfaceData*>(nullptr));
            SubjectPawn->GetCharacterMovement()->StopMovementImmediately();
            const FRotator Facing=Voyager::TangentRotation(Up,Forward);
            SubjectPawn->SetActorLocationAndRotation(Location,Facing,false,nullptr,ETeleportType::TeleportPhysics);
            SubjectPawn->GetCharacterMovement()->SetGravityDirection(-Up);SubjectPawn->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
            Controller->SetControlRotation(Facing);
            // The owning client has an autonomous movement predictor. Explicit
            // fixture teleportation must relocate it as well as the server pawn.
            Controller->ClientSetLocation(Location,Facing);Controller->ClientSetRotation(Facing,true);SubjectPawn->ForceNetUpdate();
        };
        if(TestStage==0)
        {
            AVoyagerController* Remote=nullptr;
            for(FConstPlayerControllerIterator It=GetWorld()->GetPlayerControllerIterator();It;++It)
                if(auto* Candidate=Cast<AVoyagerController>(It->Get()))
                    if(!Candidate->IsLocalController()&&Cast<AVoyagerCharacter>(Candidate->GetPawn())){Remote=Candidate;break;}
            if(!Remote)return;
            if(!Audit.Remote.IsValid()){Audit.Remote=Remote;StageStarted=TestTime;return;}
            if(TestTime-StageStarted<5.f)return;
            const FVector HostAt=AVoyagerSettlement::StreetPoint(State->SystemSeed,0,0,4,4,120.f);
            const FVector ClientAt=AVoyagerSettlement::StreetPoint(State->SystemSeed,1,1,4,4,120.f);
            Place(this,Explorer,HostAt,Voyager::SurfaceNormal(State->SystemSeed,0,HostAt),FVector::ForwardVector);
            Place(Remote,Cast<AVoyagerCharacter>(Remote->GetPawn()),ClientAt,Voyager::SurfaceNormal(State->SystemSeed,1,ClientAt),FVector::ForwardVector);
            UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY NETWORK AUDIT PASS TWO_PLANET_FIXTURE net=%d players=%d separation_km=%.1f"),Net,State->PlayerArray.Num(),FVector::Dist(HostAt,ClientAt)/100000.0);
            Advance();return;
        }
        if(TestStage==1)
        {
            const int32 HostCount=Manager->CitizensInCity(0,0),RemoteCount=Manager->CitizensInCity(1,1);
            if(HostCount<24||RemoteCount<24)
            {if(Elapsed>32.f)Fail(TEXT("TWO_CITY_POPULATION"));return;}
            if(Manager->ActiveCitizenCount()>72){Fail(TEXT("POPULATION_BUDGET"));return;}
            Audit.Positions.Empty();
            for(TActorIterator<AVoyagerCitizen> It(GetWorld());It;++It)Audit.Positions.Add(*It,It->GetActorLocation());
            UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY NETWORK AUDIT PASS AUTHORITATIVE_POPULATIONS net=%d host_city=%d remote_city=%d global=%d"),Net,HostCount,RemoteCount,Manager->ActiveCitizenCount());
            Advance();return;
        }
        if(TestStage==2&&Elapsed>6.f)
        {
            int32 Moving=0;
            for(const auto& Pair:Audit.Positions)if(Pair.Key.IsValid()&&FVector::Dist(Pair.Value,Pair.Key->GetActorLocation())>70.f)++Moving;
            if(Moving<8){Fail(TEXT("AUTHORITATIVE_MOTION"));return;}
            auto* Remote=Audit.Remote.Get();auto* RemotePawn=Remote?Cast<AVoyagerCharacter>(Remote->GetPawn()):nullptr;
            if(!RemotePawn){Fail(TEXT("REMOTE_PLAYER_LOST"));return;}
            AVoyagerCitizen* Subject=nullptr;
            for(TActorIterator<AVoyagerCitizen> It(GetWorld());It;++It)
            {
                if(It->PlanetIndex()!=1||It->SiteIndex()!=1||It->IsIndoors()||It->GetVelocity().Size()<60.f)continue;
                const FVector At=It->GetActorLocation(),Up=Voyager::SurfaceNormal(State->SystemSeed,1,At);
                Place(Remote,RemotePawn,At-It->GetActorForwardVector()*240.f+Up*95.f,Up,It->GetActorForwardVector());
                // Pause behavior for a stable fixture, but deliberately do not
                // call ShowConversation: only the real client's RPC can do that.
                if(!It->TalkTo(RemotePawn,0).IsEmpty()){Subject=*It;break;}
            }
            if(!Subject){Fail(TEXT("DIALOGUE_FIXTURE_VISIBILITY"));return;}
            Audit.Subject=Subject;
            UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY NETWORK AUDIT PASS AUTHORITATIVE_MOTION net=%d moving=%d"),Net,Moving);
            UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY NETWORK AUDIT PASS DIALOGUE_FIXTURE net=%d citizen=%s"),Net,*Subject->DisplayName());
            Advance();return;
        }
        if(TestStage==3)
        {
            int32 Alarmed=0;
            for(TActorIterator<AVoyagerCitizen> It(GetWorld());It;++It)if(It->PlanetIndex()==1&&It->SiteIndex()==1&&It->IsAlarmed())++Alarmed;
            if(Alarmed>0)
            {
                UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY NETWORK AUDIT PASS CLIENT_ALARM_AUTHORITY net=%d alarmed=%d"),Net,Alarmed);
                UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY NETWORK AUDIT COMPLETE PASS net=%d"),Net);TestStage=90;
            }
            else if(Elapsed>32.f)Fail(TEXT("CLIENT_ALARM_RPC_NOT_RECEIVED"));
        }
        return;
    }

    if(GetNetMode()!=NM_Client){Fail(TEXT("REQUIRES_NETWORK_CLIENT"));return;}
    const FVector CityCenter=Voyager::SurfacePoint(State->SystemSeed,1,AVoyagerSettlement::SiteDirection(State->SystemSeed,1,1));
    if(TestStage==0)
    {
        if(Voyager::NearestPlanet(State->SystemSeed,Explorer->GetActorLocation())!=1||FVector::Dist(Explorer->GetActorLocation(),CityCenter)>70000.f)return;
        Audit.Positions.Empty();TSet<FString> Roles;
        for(TActorIterator<AVoyagerCitizen> It(GetWorld());It;++It)
        {
            if(It->SystemIndex()!=State->SystemSeed||It->PlanetIndex()!=1||It->SiteIndex()!=1)continue;
            if(FVector::Dist(It->GetActorLocation(),CityCenter)>35000.f){Fail(TEXT("DOUBLE_COORDINATE_POSE"));return;}
            if(It->DisplayName().IsEmpty()){Fail(TEXT("CITIZEN_IDENTITY"));return;}
            Audit.Positions.Add(*It,It->GetActorLocation());Roles.Add(It->RoleName());
        }
        if(Audit.Positions.Num()<18||Roles.Num()<5||Manager->CitizensInCity(1,1)<24||Manager->CitizensInCity(0,0)<24)return;
        UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY NETWORK AUDIT PASS REMOTE_IDENTITIES_AND_POSES net=%d citizens=%d roles=%d city_radius_cm=35000"),Net,Audit.Positions.Num(),Roles.Num());
        UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY NETWORK AUDIT PASS REPLICATED_POPULATIONS net=%d host_city=%d remote_city=%d global=%d"),Net,Manager->CitizensInCity(0,0),Manager->CitizensInCity(1,1),Manager->ActiveCitizenCount());
        Advance();return;
    }
    if(TestStage==1&&Elapsed>5.f)
    {
        int32 Moving=0;
        for(const auto& Pair:Audit.Positions)if(Pair.Key.IsValid()&&FVector::Dist(Pair.Value,Pair.Key->GetActorLocation())>70.f)++Moving;
        if(Moving<4){Fail(TEXT("REPLICATED_CITIZEN_MOTION"));return;}
        UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY NETWORK AUDIT PASS REPLICATED_MOTION net=%d moving=%d observed=%d"),Net,Moving,Audit.Positions.Num());
        Advance();return;
    }
    if(TestStage==2)
    {
        if(auto* Subject=Explorer->FocusedCitizen())
        {
            if(Subject->ActivityName()!=TEXT("Speaking with you"))return;
            Audit.Subject=Subject;
            // Priming TalkTo on the server has a short anti-spam cooldown.
            if(Elapsed<2.f)return;
            SetControlRotation(Voyager::TangentRotation(Voyager::SurfaceNormal(State->SystemSeed,1,Explorer->GetActorLocation()),Subject->GetActorLocation()-Explorer->GetActorLocation()));
            Explorer->ServerInteract();Advance();return;
        }
        if(Elapsed>22.f)Fail(TEXT("NETWORK_DIALOGUE_TARGET"));return;
    }
    if(TestStage==3)
    {
        if(ConversationTime>0.f&&!ConversationSpeech.IsEmpty()&&ConversationTarget==Audit.Subject)
        {
            if(Elapsed<1.f)return;Audit.Greeting=ConversationSpeech;
            UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY NETWORK AUDIT PASS INTERACT_RPC_DIALOGUE net=%d citizen=%s chars=%d"),Net,*ConversationTarget->DisplayName(),ConversationSpeech.Len());
            Explorer->ServerTalk(1);Advance();return;
        }
        if(Elapsed>8.f)Fail(TEXT("DIALOGUE_CLIENT_RPC"));return;
    }
    if(TestStage==4)
    {
        if(ConversationTime>0.f&&!ConversationSpeech.IsEmpty()&&ConversationSpeech!=Audit.Greeting)
        {
            UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY NETWORK AUDIT PASS TOPIC_RPC_DIALOGUE net=%d chars=%d"),Net,ConversationSpeech.Len());
            Audit.ResumePosition=Audit.Subject.IsValid()?Audit.Subject->GetActorLocation():FVector::ZeroVector;
            CloseConversation();Advance();return;
        }
        if(Elapsed>8.f)Fail(TEXT("TOPIC_CLIENT_RPC"));return;
    }
    if(TestStage==5&&Elapsed>5.f)
    {
        auto* Subject=Audit.Subject.Get();
        if(!Subject||Subject->ActivityName()==TEXT("Speaking with you")||FVector::Dist(Subject->GetActorLocation(),Audit.ResumePosition)<25.f)
        {if(Elapsed>12.f)Fail(TEXT("END_CONVERSATION_RPC"));return;}
        UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY NETWORK AUDIT PASS END_CONVERSATION_RPC net=%d moved_cm=%.1f"),Net,FVector::Dist(Subject->GetActorLocation(),Audit.ResumePosition));
        Explorer->ServerMine(Explorer->GetActorForwardVector());Advance();return;
    }
    if(TestStage==6&&Elapsed>2.f)
    {
        int32 Alarmed=0;
        for(TActorIterator<AVoyagerCitizen> It(GetWorld());It;++It)if(It->PlanetIndex()==1&&It->SiteIndex()==1&&It->IsAlarmed())++Alarmed;
        if(Alarmed<1){if(Elapsed>8.f)Fail(TEXT("ALARM_STATE_REPLICATION"));return;}
        UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY NETWORK AUDIT PASS REPLICATED_ALARM net=%d alarmed=%d"),Net,Alarmed);
        UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY NETWORK AUDIT COMPLETE PASS net=%d"),Net);TestStage=90;
    }
}
