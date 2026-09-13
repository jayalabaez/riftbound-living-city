#include "VoyagerCharacter.h"
#include "VoyagerCitizen.h"
#include "VoyagerSettlement.h"
#include "VoyagerGameMode.h"
#include "VoyagerData.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Misc/CommandLine.h"

// Explicit automation fixtures relocate the camera between test cases. Normal
// travel and the doorway traversal below use the ordinary movement component.
void AVoyagerController::RunCityAudit(float DeltaSeconds)
{
    if(!HasAuthority()||TestStage>=90||TestTime<5)return;
    auto* Explorer=Cast<AVoyagerCharacter>(GetPawn());
    auto* State=GetWorld()->GetGameState<AVoyagerState>();
    if(!Explorer||!State)return;
    const double Elapsed=TestTime-StageStarted;
    auto Advance=[this](){++TestStage;StageStarted=TestTime;};
    auto Fail=[this](const TCHAR* Why)
    {UE_LOG(LogTemp,Error,TEXT("VOYAGER CITY AUDIT FAIL %s stage=%d"),Why,TestStage);TestStage=99;};
    auto Capture=[this]()
    {if(FParse::Param(FCommandLine::Get(),TEXT("VoyagerCapture")))ConsoleCommand(TEXT("HighResShot 1"));};
    auto Place=[&](FVector Location,FVector Up,FVector Forward,bool bWalking)
    {
        Explorer->SetBase(static_cast<FMovementBaseInterfaceData*>(nullptr));
        Explorer->GetCharacterMovement()->StopMovementImmediately();
        const FRotator Facing=Voyager::TangentRotation(Up,Forward);
        Explorer->SetActorLocationAndRotation(Location,Facing,false,nullptr,ETeleportType::TeleportPhysics);
        Explorer->GetCharacterMovement()->SetGravityDirection(-Up);
        Explorer->GetCharacterMovement()->SetMovementMode(bWalking?MOVE_Falling:MOVE_Flying);
        SetControlRotation(Facing);
    };
    auto Walk=[&](FVector Target,FVector Up)
    {
        const FVector Offset=FVector::VectorPlaneProject(Target-Explorer->GetActorLocation(),Up);
        if(Offset.Size()>65)Explorer->AddMovementInput(Offset.GetSafeNormal(),1);
        return Offset.Size();
    };

    // Six different rooms at the arrival settlement, then a remote spherical site.
    if(TestStage<28)
    {
        const int32 Case=TestStage/4,Phase=TestStage%4;
        const int32 Planet=Case==6?1:0,Site=Case==6?1:0,Index=Case==6?4:Case;
        FVoyagerBuildingInfo B;
        if(!AVoyagerSettlement::GetBuildingInfo(State->SystemSeed,Planet,Site,Index,B))
        {Fail(TEXT("BUILDING_DESCRIPTION"));return;}
        if(Phase==0)
        {
            CityAuditStep=0;
            Place(B.DoorOutside+B.Up*150,B.Up,B.Right,true);
            TestStart=B.DoorOutside;
            Advance();return;
        }
        if(Phase==1)
        {
            if(Elapsed<4)return;
            const double Distance=Walk(B.DoorInside+B.Up*90,B.Up);
            if(Elapsed>18)
            {
                UE_LOG(LogTemp,Error,TEXT("VOYAGER CITY AUDIT POSITION player=%s target=%s remaining=%.1f ground=%d"),*Explorer->GetActorLocation().ToString(),*B.DoorInside.ToString(),Distance,Explorer->GetCharacterMovement()->IsMovingOnGround());
                Fail(TEXT("WALK_THROUGH_DOOR"));return;
            }
            if(Distance<85)
            {
                if(!Explorer->GetCharacterMovement()->IsMovingOnGround())return;
                UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY AUDIT PASS DOOR_ENTRY case=%d role=%d planet=%d site=%d walked_cm=%.1f"),Case,B.Role,Planet,Site,FVector::Dist(TestStart,Explorer->GetActorLocation()));
                Advance();
            }
            return;
        }
        if(Phase==2)
        {
            if(Case==4&&CityAuditStep>0)
            {
                const FVector StairFoot=B.Floor+B.Forward*(B.Width*.5-150)+B.Right*(B.Depth*.5-980);
                const FVector Gallery=B.Floor+B.Forward*(B.Width*.5-150)+B.Right*(B.Depth*.5-230);
                const FVector Goal=CityAuditStep==1||CityAuditStep==3?StairFoot:CityAuditStep==2?Gallery:B.InteriorPoint;
                const double Remaining=Walk(Goal+B.Up*90,B.Up);
                if(Elapsed>12){Fail(TEXT("MEZZANINE_STAIRS_BLOCKED"));return;}
                if(Remaining<85)
                {
                    if(CityAuditStep==2)
                    {
                        const double Height=FVector::DotProduct(Explorer->GetActorLocation()-B.Floor,B.Up);
                        if(Height<300||!Explorer->GetCharacterMovement()->IsMovingOnGround())
                        {Fail(TEXT("MEZZANINE_NOT_REACHED"));return;}
                        UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY AUDIT PASS MEZZANINE_STAIRS capsule_height_cm=%.1f"),Height);
                        SetControlRotation(FRotationMatrix::MakeFromXZ((-B.Forward-B.Right*.45-B.Up*.25).GetSafeNormal(),B.Up).Rotator());
                        Capture();
                    }
                    Explorer->GetCharacterMovement()->StopMovementImmediately();
                    ++CityAuditStep;StageStarted=TestTime;
                    if(CityAuditStep>4)Advance();
                }
                return;
            }
            const double Distance=Walk(B.InteriorPoint+B.Up*90,B.Up);
            if(Elapsed>10){Fail(TEXT("INTERIOR_CORRIDOR_BLOCKED"));return;}
            if(Distance<90)
            {
                Explorer->GetCharacterMovement()->StopMovementImmediately();
                if(AVoyagerSettlement::FindBuildingAt(State->SystemSeed,Planet,Site,Explorer->GetActorLocation())!=Index)
                {Fail(TEXT("INTERIOR_VOLUME"));return;}
                SetControlRotation(Voyager::TangentRotation(B.Up,B.Forward+B.Right*.35));
                UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY AUDIT PASS WALKABLE_ROOM case=%d name=%s"),Case,*B.Name);
                Capture();
                if(Case==4){CityAuditStep=1;StageStarted=TestTime;}else Advance();
            }
            return;
        }
        if(Phase==3)
        {
            if(Elapsed<1.0)return;
            const double Distance=Walk(B.DoorOutside+B.Up*90,B.Up);
            if(Elapsed>12){Fail(TEXT("EXIT_CORRIDOR_BLOCKED"));return;}
            if(Distance<100)
            {
                if(AVoyagerSettlement::FindBuildingAt(State->SystemSeed,Planet,Site,Explorer->GetActorLocation())==Index)
                {Fail(TEXT("DID_NOT_EXIT_ROOM"));return;}
                UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY AUDIT PASS DOOR_EXIT case=%d grounded=%d"),Case,Explorer->GetCharacterMovement()->IsMovingOnGround());
                Explorer->GetCharacterMovement()->StopMovementImmediately();Advance();
            }
            return;
        }
    }
    if(TestStage==28)
    {
        if(Elapsed<2)return;
        LifeAuditPositions.Empty();
        for(TActorIterator<AVoyagerCitizen> It(GetWorld());It;++It)LifeAuditPositions.Add(*It,It->GetActorLocation());
        if(LifeAuditPositions.Num()<12||LifeAuditPositions.Num()>72){Fail(TEXT("BOUNDED_POPULATION"));return;}
        Advance();return;
    }
    if(TestStage==29&&Elapsed>7)
    {
        int32 Moving=0,Indoors=0;TSet<FString> Roles;
        for(const auto& Pair:LifeAuditPositions)
            if(auto* Citizen=Cast<AVoyagerCitizen>(Pair.Key.Get()))
            {
                if(FVector::Dist(Pair.Value,Citizen->GetActorLocation())>70)++Moving;
                if(Citizen->IsIndoors())++Indoors;
                Roles.Add(Citizen->RoleName());
            }
        if(Moving<4||Roles.Num()<4||Indoors<1)
        {
            UE_LOG(LogTemp,Error,TEXT("VOYAGER CITY AUDIT POPULATION moving=%d roles=%d indoors=%d"),Moving,Roles.Num(),Indoors);
            Fail(TEXT("CITIZEN_ROUTINES"));return;
        }
        UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY AUDIT PASS CITIZEN_ROUTINES citizens=%d moving=%d indoors=%d roles=%d"),LifeAuditPositions.Num(),Moving,Indoors,Roles.Num());
        LifeAuditPositions.Empty();Advance();return;
    }
    if(TestStage==30)
    {
        AVoyagerCitizen* Subject=nullptr;
        for(TActorIterator<AVoyagerCitizen> It(GetWorld());It;++It)
            if(It->PlanetIndex()==1&&It->SiteIndex()==1&&!It->IsIndoors()){Subject=*It;break;}
        if(!Subject){Fail(TEXT("CITIZEN_TO_TALK_TO"));return;}
        CityAuditCitizen=Subject;
        const FVector Up=Subject->GetActorUpVector();
        Place(Subject->GetActorLocation()+Subject->GetActorForwardVector()*240+Up*90,Up,-Subject->GetActorForwardVector(),false);
        Subject->TalkTo(Explorer,0);
        Advance();return;
    }
    if(TestStage==31&&Elapsed>1)
    {
        Explorer->ServerInteract();Advance();return;
    }
    if(TestStage==32&&Elapsed>1)
    {
        if(ConversationTime<=0||!ConversationTarget.IsValid()||ConversationSpeech.IsEmpty())
        {Fail(TEXT("CONTEXTUAL_CONVERSATION"));return;}
        UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY AUDIT PASS CONVERSATION name=%s reply=%s"),*ConversationTarget->DisplayName(),*ConversationSpeech);
        Capture();Explorer->ServerTalk(1);Advance();return;
    }
    if(TestStage==33&&Elapsed>1)
    {
        auto* Subject=CityAuditCitizen.Get();
        if(!Subject){Fail(TEXT("CITIZEN_DISAPPEARED"));return;}
        if(!Subject->TalkTo(Explorer,99).IsEmpty()){Fail(TEXT("INVALID_TOPIC_ACCEPTED"));return;}
        const FVector Original=Explorer->GetActorLocation();
        Explorer->SetActorLocation(Original+Subject->GetActorForwardVector()*2000);
        if(!Subject->TalkTo(Explorer,0).IsEmpty()){Fail(TEXT("REMOTE_TALK_ACCEPTED"));return;}
        Explorer->SetActorLocation(Original);CloseConversation();
        for(TActorIterator<AVoyagerCitizenManager> It(GetWorld());It;++It)It->ReportDisturbance(Subject->GetActorLocation());
        UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY AUDIT PASS INTERACTION_VALIDATION"));
        Advance();return;
    }
    if(TestStage==34&&Elapsed>2)
    {
        int32 Alarmed=0;
        for(TActorIterator<AVoyagerCitizen> It(GetWorld());It;++It)if(It->IsAlarmed())++Alarmed;
        if(Alarmed<1){Fail(TEXT("NO_DANGER_REACTION"));return;}
        UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY AUDIT PASS DANGER_REACTION alerted=%d"),Alarmed);
        Capture();Advance();return;
    }
    if(TestStage==35&&Elapsed>1)
    {
        UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY AUDIT COMPLETE PASS"));TestStage=90;
    }
}
