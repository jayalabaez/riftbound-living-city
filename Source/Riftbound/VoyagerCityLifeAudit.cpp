// Opt-in integration checks use the real worker, phone bindings and save path.
#include "VoyagerCityLife.h"
#include "VoyagerCharacter.h"
#include "VoyagerCitizen.h"
#include "VoyagerGameMode.h"
#include "VoyagerSettlement.h"
#include "VoyagerLaw.h"
#include "VoyagerData.h"
#include "Components/InputComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "Engine/DamageEvents.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

namespace VoyagerCityLifeAudit
{
struct FRun
{
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<AVoyagerController> PC,Peer;
    TWeakObjectPtr<AVoyagerCitizen> DeadCitizen;
    FVoyagerCityLifeView SavedPlayer;
    double Started=0,StageStarted=0,NextPoll=0,PreviousFrame=0,FrameSeconds=0,WorstFrameMs=0;
    uint64 FrameCount=0;
    int32 Stage=0,Checks=0,Planet=0,Site=0,Market=2,Employer=3,Civic=5;
    int32 BeforeOwned=0,BeforeNeed=0,BeforeMinerals=0,BeforeRecord=0,InitialSystem=0,NewHome=INDEX_NONE;
    int32 DeadOrdinal=INDEX_NONE,DeadHome=INDEX_NONE,DeadWork=INDEX_NONE;
    int64 BeforeMoney=0,BeforeTick=0,BeforeFine=0,WorkTick=0,PeerMoney=0,IssuedMoney=0,BeforeRent=0,BeforeBills=0;
    float PeakTickMs=0;
    bool bFinished=false,bNetwork=false,bPeerTracked=false,bCaptured=false;
};
TUniquePtr<FRun> Run;
void Pass(const TCHAR* Name)
{++Run->Checks;UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY LIFE AUDIT PASS %s"),Name);}
void Fail(const TCHAR* Name)
{Run->bFinished=true;UE_LOG(LogTemp,Error,TEXT("VOYAGER CITY LIFE AUDIT FAIL stage=%d %s"),Run->Stage,Name);FPlatformMisc::RequestExit(false);}
void Advance(double Now,int32 Stage=INDEX_NONE)
{Run->Stage=Stage==INDEX_NONE?Run->Stage+1:Stage;Run->StageStarted=Now;}
bool PressKey(AVoyagerController* PC,FKey Key)
{
    if(!PC||!PC->InputComponent)return false;
    for(const auto& Binding:PC->InputComponent->KeyBindings)
        if(Binding.KeyEvent==IE_Pressed&&Binding.Chord.Key==Key&&Binding.KeyDelegate.IsBound())
        {Binding.KeyDelegate.Execute(Key);return true;}
    return false;
}
void Capture(const TCHAR* Name)
{
    if(!FParse::Param(FCommandLine::Get(),TEXT("VoyagerCityLifeCapture")))return;
    FString Folder;if(!FParse::Value(FCommandLine::Get(),TEXT("VoyagerCityLifeOutput="),Folder))Folder=FPaths::ProjectSavedDir()/TEXT("VoyagerCityLifeAudit");
    IFileManager::Get().MakeDirectory(*Folder,true);
    FScreenshotRequest::RequestScreenshot(Folder/(FString(Name)+TEXT(".png")),true,false);
}
void Place(AVoyagerCharacter* Pawn,FVector Location,FVector Forward)
{
    auto State=Pawn->GetWorld()->GetGameState<AVoyagerState>();
    const FVector Up=Voyager::SurfaceNormal(State->SystemSeed,Run->Planet,Location);
    Pawn->GetCharacterMovement()->StopMovementImmediately();Pawn->GetCharacterMovement()->SetGravityDirection(-Up);
    Pawn->SetActorLocationAndRotation(Location,Voyager::TangentRotation(Up,Forward),false,nullptr,ETeleportType::TeleportPhysics);
    Run->PC->SetControlRotation(Voyager::TangentRotation(Up,Forward));
    // Only relocation is a fixture. Commands still resolve the actual containing building.
    Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
}
bool PlaceBuilding(AVoyagerCharacter* Pawn,int32 Building)
{
    FVoyagerBuildingInfo Info;
    if(!AVoyagerSettlement::GetBuildingInfo(Run->InitialSystem,Run->Planet,Run->Site,Building,Info))return false;
    Place(Pawn,Info.InteriorPoint+Info.Up*115,Info.Forward);return true;
}
void PlaceOutside(AVoyagerCharacter* Pawn)
{
    const FVector Up=AVoyagerSettlement::SiteDirection(Run->InitialSystem,Run->Planet,Run->Site);
    const FRotator Frame=Voyager::TangentRotation(Up);
    Place(Pawn,Voyager::SurfacePoint(Run->InitialSystem,Run->Planet,Up,115)+Frame.Vector()*2000,Frame.Vector());
}
void Command(AVoyagerController* PC,EVoyagerCityAction Action,int32 Argument=0)
{PC->ServerCityAction(static_cast<uint8>(Action),Argument);}
bool ChangedSnapshot(const FVoyagerCityLifeView& V,double Elapsed)
{return Elapsed>=1.2&&V.Tick>Run->BeforeTick;}
void Remember(const FVoyagerCityLifeView& V)
{Run->BeforeMoney=V.CreditsMinor;Run->BeforeTick=V.Tick;Run->BeforeOwned=V.Goods[0].Owned;Run->BeforeNeed=V.Needs[0];Run->BeforeFine=V.FineMinor;Run->BeforeRecord=V.CriminalRecord;}
bool ReportWitnessedCrime(AVoyagerLaw* Law,AVoyagerCharacter* Pawn)
{
    auto PS=Run->PC->GetPlayerState<AVoyagerPlayerState>();
    for(TActorIterator<AVoyagerCitizen> It(Pawn->GetWorld());It;++It)
    {
        if(!It->IsAlive()||It->PlanetIndex()!=Run->Planet||It->SiteIndex()!=Run->Site)continue;
        const FVector Up=It->GetActorUpVector(),At=It->GetActorLocation();
        for(int32 Side=-1;Side<=1;Side+=2)
        {
            Place(Pawn,At+It->GetActorRightVector()*(Side*220)+Up*115,It->GetActorRightVector()*-Side);
            Law->ReportCrime(Run->PC.Get(),Pawn->GetActorLocation(),20,TEXT("integration audit observed incident"),*It);
            if(PS->WantedStars>0)return true;
        }
    }
    return false;
}

void Tick(UWorld* World,ELevelTick Type,float Delta)
{
    if(!FParse::Param(FCommandLine::Get(),TEXT("VoyagerCityLifeAudit"))||!World||!World->IsGameWorld())return;
    if(!Run)
    {
        if(World->GetTimeSeconds()<8.f)return;
        auto PC=Cast<AVoyagerController>(World->GetFirstPlayerController());
        if(!PC||!PC->IsLocalController()||!Cast<AVoyagerCharacter>(PC->GetPawn()))return;
        Run=MakeUnique<FRun>();Run->World=World;Run->PC=PC;
        Run->Started=Run->StageStarted=FPlatformTime::Seconds();Run->bNetwork=World->GetNetMode()==NM_ListenServer;
    }
    if(Run->World.Get()!=World||Run->bFinished)return;
    const double Now=FPlatformTime::Seconds(),Elapsed=Now-Run->StageStarted;
    // Count every actual world frame, including UI/capture/save overhead. Delta can
    // be clamped by the engine and is deliberately not the timing source here.
    if(Run->PreviousFrame>0&&Now>Run->PreviousFrame)
    {
        const double FrameTime=Now-Run->PreviousFrame;
        ++Run->FrameCount;Run->FrameSeconds+=FrameTime;Run->WorstFrameMs=FMath::Max(Run->WorstFrameMs,FrameTime*1000.0);
    }
    Run->PreviousFrame=Now;
    if(Now-Run->Started>300){Fail(TEXT("TIMEOUT"));return;}
    if(Now<Run->NextPoll)return;Run->NextPoll=Now+.5;
    auto PC=Run->PC.Get();auto Pawn=PC?Cast<AVoyagerCharacter>(PC->GetPawn()):nullptr;
    auto PS=PC?PC->GetPlayerState<AVoyagerPlayerState>():nullptr;
    auto State=World->GetGameState<AVoyagerState>();
    if(!PC||!Pawn||!PS||!State)return;
    const auto V=PC->CityLifeView;
    if(!V.bAvailable||V.Goods.Num()!=8||V.Needs.Num()!=9)return;
    Run->PeakTickMs=FMath::Max(Run->PeakTickMs,V.TickMilliseconds);
    if(World->GetNetMode()==NM_Client)
    {
        if(Run->Stage==0)
        {
            if(V.SystemPopulation!=54000||V.ResidentId<3596){Fail(TEXT("REMOTE_SNAPSHOT_INVALID"));return;}
            Pass(TEXT("REMOTE_CITY_SNAPSHOT"));Remember(V);
            if(!PressKey(PC,EKeys::P)||!PC->bCityPhoneVisible){Fail(TEXT("REMOTE_PHONE_INPUT"));return;}
            Pass(TEXT("REMOTE_PHONE_OPENS"));PressKey(PC,EKeys::P);Advance(Now);return;
        }
        if(Elapsed<90)return;
        if(V.Tick<=Run->BeforeTick+200||V.CreditsMinor!=Run->BeforeMoney||V.FineMinor!=Run->BeforeFine||V.CriminalRecord!=Run->BeforeRecord)
        {Fail(TEXT("REMOTE_ACCOUNT_CHANGED_BY_HOST"));return;}
        Pass(TEXT("REMOTE_LIVE_CLOCK"));Pass(TEXT("REMOTE_ACCOUNT_UNTOUCHED"));
        Run->bFinished=true;UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY LIFE AUDIT CLIENT COMPLETE PASS"));FPlatformMisc::RequestExit(false);return;
    }
    auto City=AVoyagerCityLife::Find(World);auto Law=AVoyagerLaw::Find(World);if(!City||!City->IsReady()||!Law)return;
    if(Run->bNetwork&&!Run->bPeerTracked)
    {
        for(FConstPlayerControllerIterator It=World->GetPlayerControllerIterator();It;++It)
        {
            auto Peer=Cast<AVoyagerController>(It->Get());if(!Peer||Peer==PC||!Peer->GetPawn())continue;
            const auto PeerView=City->BuildView(Peer);
            if(!PeerView.bAvailable||PeerView.ResidentId==V.ResidentId)continue;
            Run->Peer=Peer;Run->PeerMoney=PeerView.CreditsMinor;Run->bPeerTracked=true;Pass(TEXT("DISTINCT_COOP_RESIDENTS"));break;
        }
    }
    switch(Run->Stage)
    {
    case 0:
    {
        if(!V.bAtCity)return;
        if(V.SystemPopulation!=54000||V.Population!=3600||V.CityKey<0||V.CityKey>=15||V.HomeName.IsEmpty()||V.WorkName.IsEmpty())
        {Fail(TEXT("CITY_SNAPSHOT_INVALID"));return;}
        Run->InitialSystem=State->SystemSeed;Run->Planet=V.CityKey/3;Run->Site=V.CityKey%3;
        int64 Total=0;
        if(!City->AuditConservation(Total,Run->IssuedMoney)||Total!=Run->IssuedMoney){Fail(TEXT("INITIAL_MONEY_NOT_CONSERVED"));return;}
        Pass(TEXT("INITIAL_MONEY_CONSERVED"));
        Pass(TEXT("FIFTY_FOUR_THOUSAND_RESIDENTS"));Pass(TEXT("NINE_NEEDS_EIGHT_GOODS"));Pass(TEXT("OWNER_SNAPSHOT_DELIVERED"));
        if(!PressKey(PC,EKeys::P)||!PC->bCityPhoneVisible||!PC->bMenuVisible||!PC->IsMoveInputIgnored()||!PC->IsLookInputIgnored())
        {Fail(TEXT("PHONE_MODAL_INPUT"));return;}
        const bool WeaponMode=Pawn->bWeaponMode;Pawn->ServerToggleWeapon();
        if(Pawn->bWeaponMode!=WeaponMode){Fail(TEXT("PHONE_ALLOWS_WEAPON_ACTION"));return;}
        Pass(TEXT("PHONE_BLOCKS_WEAPON_ACTION"));
        if(PC->CityGuideTarget!=1){Fail(TEXT("NAVIGATION_NOT_DEFAULT_HOME"));return;}
        for(int32 I=0;I<5;++I)
        {
            const int32 Expected=(PC->CityGuideTarget+1)%5;
            if(!PressKey(PC,EKeys::N)||PC->CityGuideTarget!=Expected){Fail(TEXT("NAVIGATION_KEY_CYCLE"));return;}
        }
        Pass(TEXT("N_KEY_CYCLES_ALL_DESTINATIONS"));
        PC->CityPhonePage=2;
        if(!PressKey(PC,EKeys::Eight)||PC->CityGuideTarget!=2){Fail(TEXT("PHONE_NAVIGATION_ACTION"));return;}
        Pass(TEXT("PHONE_EIGHT_CYCLES_NAVIGATION"));
        for(int32 I=0;I<4;++I)PressKey(PC,EKeys::N);
        PC->NoticeTime=0;PC->CityPhonePage=0;Pass(TEXT("PHONE_MODAL_INPUT"));Advance(Now);return;
    }
    case 1:case 2:case 3:case 4:case 5:
    {
        if(Elapsed<1.5)return;
        static const TCHAR* Names[]={TEXT("Overview"),TEXT("Market"),TEXT("WorkHome"),TEXT("Justice"),TEXT("News")};
        if(PC->CityPhonePage!=Run->Stage-1){Fail(TEXT("PHONE_PAGE_BINDING"));return;}
        if(!Run->bCaptured){Capture(Names[Run->Stage-1]);Run->bCaptured=true;return;}
        Run->bCaptured=false;
        if(Run->Stage<5){if(!PressKey(PC,EKeys::Right)){Fail(TEXT("PHONE_ARROW_BINDING"));return;}}
        Advance(Now);return;
    }
    case 6:
    {
        if(Elapsed<1)return;
        if(!PressKey(PC,EKeys::P)||PC->bCityPhoneVisible||PC->bMenuVisible||PC->IsMoveInputIgnored()||PC->IsLookInputIgnored())
        {Fail(TEXT("PHONE_CLOSE_RESTORES_INPUT"));return;}
        Pass(TEXT("FIVE_PHONE_PAGES"));Pass(TEXT("PHONE_CLOSE_RESTORES_INPUT"));
        PS->Minerals=100;PS->ForceNetUpdate();PlaceOutside(Pawn);
        FVoyagerBuildingInfo Home;
        if(!AVoyagerSettlement::GetBuildingInfo(Run->InitialSystem,Run->Planet,Run->Site,V.HomeBuilding,Home))
        {Fail(TEXT("NAVIGATION_HOME_ADDRESS"));return;}
        PC->SetControlRotation(Voyager::TangentRotation(Home.Up,(Home.DoorOutside+Home.Up*150-Pawn->GetPawnViewLocation()).GetSafeNormal()));
        PC->NoticeTime=0;Advance(Now,60);return;
    }
    case 60:
        if(Elapsed<2)return;
        if(PC->CityGuideTarget!=1||PC->bCityPhoneVisible){Fail(TEXT("GROUND_NAVIGATION_STATE"));return;}
        Capture(TEXT("Navigation"));Advance(Now,61);return;
    case 61:
        if(Elapsed<.5)return;Advance(Now,7);return;
    case 7:
        if(Elapsed<1.5||V.CurrentBuilding!=INDEX_NONE)return;
        Remember(V);Run->BeforeMinerals=PS->Minerals;
        PressKey(PC,EKeys::P);PC->CityPhonePage=1;
        if(!PressKey(PC,EKeys::One)){Fail(TEXT("PHONE_MARKET_BINDING"));return;}
        Advance(Now);return;
    case 8:
        if(!ChangedSnapshot(V,Elapsed))return;
        if(V.CreditsMinor!=Run->BeforeMoney||V.Goods[0].Owned!=Run->BeforeOwned){Fail(TEXT("OUTSIDE_PURCHASE_DEBITED"));return;}
        Pass(TEXT("OUTSIDE_PURCHASE_REJECTED"));Remember(V);Command(PC,EVoyagerCityAction::SellMinerals,10);Advance(Now);return;
    case 9:
        if(!ChangedSnapshot(V,Elapsed))return;
        if(PS->Minerals!=Run->BeforeMinerals||V.CreditsMinor!=Run->BeforeMoney){Fail(TEXT("REJECTED_TRADE_LOST_CARGO"));return;}
        Pass(TEXT("REJECTED_TRADE_PRESERVES_CARGO"));PressKey(PC,EKeys::P);
        if(!PlaceBuilding(Pawn,Run->Market)){Fail(TEXT("MARKET_FIXTURE"));return;}Advance(Now);return;
    case 10:
        if(Elapsed<1.5||V.CurrentBuilding!=Run->Market)return;
        Remember(V);PressKey(PC,EKeys::P);PC->CityPhonePage=1;PressKey(PC,EKeys::One);Advance(Now);return;
    case 11:
        if(!ChangedSnapshot(V,Elapsed))return;
        if(V.Goods[0].Owned!=Run->BeforeOwned+1||V.CreditsMinor>=Run->BeforeMoney){Fail(TEXT("MARKET_PURCHASE_NOT_TRANSFERRED"));return;}
        UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY LIFE AUDIT TRANSACTION food_debit=%lld inventory=%d tick=%lld"),Run->BeforeMoney-V.CreditsMinor,V.Goods[0].Owned,V.Tick);
        Pass(TEXT("MARKET_PURCHASE_TRANSFERS_CREDITS"));Remember(V);PC->CityPhonePage=0;PressKey(PC,EKeys::One);Advance(Now);return;
    case 12:
        if(!ChangedSnapshot(V,Elapsed))return;
        if(V.Goods[0].Owned!=Run->BeforeOwned-1||V.Needs[0]>=Run->BeforeNeed||V.CreditsMinor!=Run->BeforeMoney)
        {Fail(TEXT("FOOD_CONSUMPTION_STATE"));return;}
        Pass(TEXT("FOOD_CONSUMED_AND_NEED_SATISFIED"));Remember(V);Run->BeforeMinerals=PS->Minerals;
        PC->CityPhonePage=2;PressKey(PC,EKeys::Six);Advance(Now);return;
    case 13:
        if(!ChangedSnapshot(V,Elapsed))return;
        if(PS->Minerals!=Run->BeforeMinerals-10||V.CreditsMinor<=Run->BeforeMoney){Fail(TEXT("MINERAL_ESCROW_TRANSFER"));return;}
        if(Run->bNetwork)
        {
            if(!Run->bPeerTracked||!Run->Peer.IsValid())return;
            const auto PeerView=City->BuildView(Run->Peer.Get());
            if(PeerView.CreditsMinor!=Run->PeerMoney||PeerView.ResidentId==V.ResidentId){Fail(TEXT("HOST_TRADE_CHANGED_PEER_ACCOUNT"));return;}
            Pass(TEXT("HOST_TRADE_ISOLATED_FROM_PEER"));
        }
        Pass(TEXT("MINERAL_TRADE_DEDUCTS_EXACTLY_TEN"));
        PressKey(PC,EKeys::P);if(!PlaceBuilding(Pawn,Run->Employer)){Fail(TEXT("EMPLOYER_FIXTURE"));return;}Advance(Now);return;
    case 14:
        if(Elapsed<1.5||V.CurrentBuilding!=Run->Employer)return;
        Remember(V);Command(PC,EVoyagerCityAction::ApplyJob);Advance(Now);return;
    case 15:
        if(!ChangedSnapshot(V,Elapsed))return;
        if(!V.bEmployed||V.WorkplaceBuilding!=Run->Employer){Fail(TEXT("JOB_APPLICATION_REJECTED"));return;}
        Pass(TEXT("JOB_AT_REAL_WORKPLACE"));Remember(V);Command(PC,EVoyagerCityAction::StartWork);Advance(Now);return;
    case 16:
        if(!ChangedSnapshot(V,Elapsed))return;
        if(!V.bWorking){Fail(TEXT("WORK_SHIFT_NOT_STARTED"));return;}
        Run->WorkTick=V.Tick;Remember(V);Pass(TEXT("WORK_SHIFT_STARTED"));Advance(Now);return;
    case 17:
        if(V.Tick<Run->WorkTick+1200)return;
        if(!V.bWorking||V.CreditsMinor<=Run->BeforeMoney){Fail(TEXT("COMPLETED_HOUR_NO_WAGES"));return;}
        UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY LIFE AUDIT WAGES earned_minor=%lld work_ticks=%lld elapsed=%.2f"),V.CreditsMinor-Run->BeforeMoney,V.Tick-Run->WorkTick,Elapsed);
        Pass(TEXT("REAL_HOUR_EARNS_WAGES"));Command(PC,EVoyagerCityAction::StopWork);Advance(Now);return;
    case 18:
        if(Elapsed<1.5)return;
        if(V.bWorking){Fail(TEXT("SHIFT_DID_NOT_END"));return;}
        PlaceOutside(Pawn);Advance(Now);return;
    case 19:
        if(Elapsed<1.5||V.CurrentBuilding!=INDEX_NONE)return;
        Remember(V);Command(PC,EVoyagerCityAction::StartWork);Advance(Now);return;
    case 20:
        if(!ChangedSnapshot(V,Elapsed))return;
        if(V.bWorking||V.CreditsMinor!=Run->BeforeMoney){Fail(TEXT("WORK_ALLOWED_OUTSIDE_EMPLOYER"));return;}
        Pass(TEXT("WORK_REQUIRES_REAL_WORKPLACE"));Remember(V);
        City->RecordCrime(PC,20,300);Advance(Now);return;
    case 21:
        if(!ChangedSnapshot(V,Elapsed))return;
        if(V.FineMinor!=Run->BeforeFine||V.CriminalRecord!=Run->BeforeRecord){Fail(TEXT("UNCONFIRMED_EVIDENCE_CREATED_FINE"));return;}
        Pass(TEXT("LOW_CONFIDENCE_EVIDENCE_REJECTED"));Remember(V);
        if(!ReportWitnessedCrime(Law,Pawn)){Fail(TEXT("NO_OBSERVER_CAN_SEE_SUSPECT"));return;}
        Pass(TEXT("ACTUAL_WITNESS_STARTS_PURSUIT"));Advance(Now);return;
    case 22:
        if(!ChangedSnapshot(V,Elapsed))return;
        if(V.FineMinor<=Run->BeforeFine||V.CriminalRecord!=Run->BeforeRecord+1){Fail(TEXT("OBSERVED_OFFENSE_MISSING"));return;}
        Pass(TEXT("OBSERVED_OFFENSE_HAS_FINE_AND_RECORD"));PlaceOutside(Pawn);Advance(Now,220);return;
    case 220:
        if(Elapsed<1.5||V.CurrentBuilding!=INDEX_NONE)return;
        Remember(V);Command(PC,EVoyagerCityAction::PayFine);Advance(Now,23);return;
    case 23:
        if(!ChangedSnapshot(V,Elapsed))return;
        if(V.FineMinor!=Run->BeforeFine||V.CreditsMinor!=Run->BeforeMoney){Fail(TEXT("FINE_PAID_OUTSIDE_CIVIC_BUILDING"));return;}
        Pass(TEXT("FINE_PAYMENT_REQUIRES_CIVIC_BUILDING"));
        if(!PlaceBuilding(Pawn,Run->Civic)){Fail(TEXT("CIVIC_FIXTURE"));return;}Advance(Now);return;
    case 24:
        if(Elapsed<1.5||V.CurrentBuilding!=Run->Civic)return;
        Remember(V);Command(PC,EVoyagerCityAction::PayFine);Advance(Now);return;
    case 25:
        if(!ChangedSnapshot(V,Elapsed))return;
        if(V.FineMinor!=0||V.CriminalRecord!=Run->BeforeRecord||V.CreditsMinor!=Run->BeforeMoney-Run->BeforeFine)
        {Fail(TEXT("FINE_PAYMENT_NOT_EXACT_OR_RECORD_ERASED"));return;}
        Pass(TEXT("FINE_PAYMENT_EXACT_RECORD_RETAINED"));
        if(PS->WantedStars!=0||Law->PatrolCount(PC)!=0){Fail(TEXT("SETTLED_FINE_LEFT_ACTIVE_PURSUIT"));return;}
        Pass(TEXT("SETTLED_FINE_NO_PURSUIT"));
        if(!PlaceBuilding(Pawn,V.HomeBuilding)){Fail(TEXT("HOME_FIXTURE"));return;}Advance(Now,250);return;
    case 250:
        if(Elapsed<1.5||V.CurrentBuilding!=V.HomeBuilding)return;
        Remember(V);Run->BeforeNeed=V.Needs[2];Command(PC,EVoyagerCityAction::Wash);Advance(Now,251);return;
    case 251:
        if(!ChangedSnapshot(V,Elapsed))return;
        if(V.Needs[2]>=Run->BeforeNeed||V.CreditsMinor!=Run->BeforeMoney){Fail(TEXT("HOME_WASH_NOT_EFFECTIVE"));return;}
        Pass(TEXT("HOME_WASH_SATISFIES_HYGIENE"));Remember(V);Run->BeforeNeed=V.Needs[3];Run->WorkTick=V.Tick;
        Command(PC,EVoyagerCityAction::Rest);Advance(Now,252);return;
    case 252:
    {
        if(Elapsed<2.5||V.Tick<Run->WorkTick+40)return;
        if(V.Needs[3]>=Run->BeforeNeed){Fail(TEXT("HOME_REST_NOT_EFFECTIVE"));return;}
        Pass(TEXT("HOME_REST_REDUCES_PRESSURE"));
        for(int32 Index=0;Index<AVoyagerSettlement::BuildingCount();++Index)
        {
            FVoyagerBuildingInfo Info;
            if(Index!=V.HomeBuilding&&AVoyagerSettlement::GetBuildingInfo(Run->InitialSystem,Run->Planet,Run->Site,Index,Info)&&Info.Role==4)
            {Run->NewHome=Index;break;}
        }
        if(Run->NewHome==INDEX_NONE||!PlaceBuilding(Pawn,Run->NewHome)){Fail(TEXT("ALTERNATE_RESIDENCE_FIXTURE"));return;}
        Advance(Now,253);return;
    }
    case 253:
        if(Elapsed<1.5||V.CurrentBuilding!=Run->NewHome)return;
        Remember(V);Run->BeforeRent=V.RentMinor;Command(PC,EVoyagerCityAction::RentHome);Advance(Now,254);return;
    case 254:
        if(!ChangedSnapshot(V,Elapsed))return;
        if(V.HomeBuilding!=Run->NewHome||V.CreditsMinor!=Run->BeforeMoney-Run->BeforeRent)
        {Fail(TEXT("RENT_ADDRESS_OR_DEBIT_INCORRECT"));return;}
        Pass(TEXT("RENT_CHANGES_ADDRESS_AND_TRANSFERS_CREDITS"));Remember(V);Run->BeforeBills=V.BillsMinor;
        PC->Notice.Empty();PC->NoticeTime=0;Command(PC,EVoyagerCityAction::PayBills);Advance(Now,255);return;
    case 255:
    {
        if(!ChangedSnapshot(V,Elapsed))return;
        // This short scenario does not cross a billing day. Only the home action
        // acknowledgement and exact outstanding amount are covered here.
        if(V.BillsMinor!=0||!V.bUtilitiesConnected||V.CreditsMinor!=Run->BeforeMoney-Run->BeforeBills||PC->Notice!=TEXT("Transaction completed."))
        {Fail(TEXT("HOME_BILL_ACTION_NOT_ACCEPTED"));return;}
        Pass(TEXT("HOME_BILL_ACTION_ACCEPTED"));
        for(TActorIterator<AVoyagerCitizen> It(World);It;++It)
        {
            if(!It->IsAlive()||It->PlanetIndex()!=Run->Planet||It->SiteIndex()!=Run->Site)continue;
            FVoyagerCitizenLifeView Resident;
            if(!City->GetCitizen(Run->InitialSystem,Run->Planet,Run->Site,It->OrdinalIndex(),Resident)||!Resident.bReady||!Resident.bAlive)continue;
            if(It->HomeBuildingIndex()!=Resident.Home||It->WorkplaceBuildingIndex()!=Resident.Workplace)
            {Fail(TEXT("VISIBLE_CITIZEN_IDENTITY_DIFFERS_FROM_CORE"));return;}
            Run->DeadCitizen=*It;Run->DeadOrdinal=It->OrdinalIndex();Run->DeadHome=Resident.Home;Run->DeadWork=Resident.Workplace;
            Pass(TEXT("VISIBLE_CITIZEN_HOME_WORK_MATCH_CORE"));
            Place(Pawn,It->GetActorLocation()+It->GetActorRightVector()*220+It->GetActorUpVector()*115,-It->GetActorRightVector());
            FDamageEvent Damage;It->TakeDamage(200,Damage,PC,Pawn);
            if(It->IsAlive()){Fail(TEXT("CITIZEN_FATAL_DAMAGE_REJECTED"));return;}
            Advance(Now,256);return;
        }
        Fail(TEXT("NO_CITIZEN_FOR_PERSISTENCE_FIXTURE"));return;
    }
    case 256:
    {
        if(Elapsed<1.5)return;
        FVoyagerCitizenLifeView Resident;
        if(!City->GetCitizen(Run->InitialSystem,Run->Planet,Run->Site,Run->DeadOrdinal,Resident)||Resident.bAlive||Resident.Health!=0)
        {Fail(TEXT("PHYSICAL_DEATH_NOT_IN_CORE"));return;}
        Pass(TEXT("CITIZEN_FATAL_DAMAGE_REACHES_CORE"));
        Run->SavedPlayer=City->BuildView(PC);PC->ServerSave();Advance(Now,26);return;
    }
    case 26:
    {
        if(Elapsed<2)return;
        if(Run->bNetwork&&Now-Run->Started<110)return;
        auto Save=Cast<UVoyagerSave>(UGameplayStatics::LoadGameFromSlot(TEXT("Voyager-Automation-CityLife"),0));
        if(!Save||Save->CityArchives.Num()!=15||Save->CityResidentLocations.Num()!=4||Save->Minerals!=PS->Minerals)
        {Fail(TEXT("CITY_ARCHIVES_NOT_SAVED"));return;}
        TSet<int32> Keys;int64 Bytes=0,ReadBackTotal=0,ReadBackIssued=0;bool bPlayerRestored=false,bDeadResidentRestored=false;
        for(const auto& Archive:Save->CityArchives)
        {
            if(Archive.System!=State->SystemSeed||Archive.Planet<0||Archive.Planet>=5||Archive.Site<0||Archive.Site>=3||(Archive.State.IsEmpty()&&Archive.PackedState.IsEmpty()))
            {Fail(TEXT("INVALID_CITY_ARCHIVE"));return;}
            const int32 Key=Archive.Planet*3+Archive.Site;
            Keys.Add(Key);Bytes+=Archive.State.Num()+Archive.PackedState.Len();
            FVoyagerCitizenLifeView Restored;int64 ArchiveTotal=0,ArchiveIssued=0;
            if(!AVoyagerCityLife::ValidateArchive(Archive,Run->SavedPlayer.ResidentId,Restored,ArchiveTotal,ArchiveIssued)||!Restored.bReady||ArchiveTotal!=ArchiveIssued)
            {Fail(TEXT("ARCHIVE_CORE_RELOAD_OR_CONSERVATION_FAILED"));return;}
            ReadBackTotal+=ArchiveTotal;ReadBackIssued+=ArchiveIssued;
            if(Key==Run->SavedPlayer.CityKey)
            {
                if(!Restored.bAlive||Restored.Home!=Run->SavedPlayer.HomeBuilding||Restored.Workplace!=Run->SavedPlayer.WorkplaceBuilding||
                    Restored.MoneyMinor!=Run->SavedPlayer.CreditsMinor||Restored.FineMinor!=Run->SavedPlayer.FineMinor)
                {Fail(TEXT("PLAYER_ARCHIVE_STATE_DID_NOT_ROUND_TRIP"));return;}
                bPlayerRestored=true;
            }
            if(Key==Run->Planet*3+Run->Site)
            {
                FVoyagerCitizenLifeView Dead;
                if(!AVoyagerCityLife::ValidateArchive(Archive,Run->DeadOrdinal,Dead,ArchiveTotal,ArchiveIssued)||!Dead.bReady||Dead.bAlive||Dead.Health!=0||
                    Dead.Home!=Run->DeadHome||Dead.Workplace!=Run->DeadWork)
                {Fail(TEXT("CITIZEN_DEATH_DID_NOT_ROUND_TRIP"));return;}
                bDeadResidentRestored=true;
            }
        }
        if(Keys.Num()!=15){Fail(TEXT("DUPLICATE_CITY_ARCHIVES"));return;}
        Pass(TEXT("FIFTEEN_CITY_ARCHIVES_ON_DISK"));
        if(!bPlayerRestored||!bDeadResidentRestored||ReadBackTotal!=ReadBackIssued||ReadBackIssued!=Run->IssuedMoney)
        {Fail(TEXT("SAVED_CORE_STATE_INCOMPLETE"));return;}
        Pass(TEXT("FIFTEEN_ARCHIVES_RELOAD_AND_CONSERVE"));Pass(TEXT("PLAYER_WALLET_HOME_FINE_ROUND_TRIP"));Pass(TEXT("NPC_DEATH_HOME_WORK_ROUND_TRIP"));
        FVoyagerCityArchive Corrupt=Save->CityArchives[0];Corrupt.State.Reset();Corrupt.PackedState=TEXT("!!!!invalid-city-archive!!!!");
        FVoyagerCitizenLifeView RejectedResident;int64 RejectedTotal=0,RejectedIssued=0;
        if(AVoyagerCityLife::ValidateArchive(Corrupt,Run->SavedPlayer.ResidentId,RejectedResident,RejectedTotal,RejectedIssued))
        {Fail(TEXT("CORRUPT_PACKED_ARCHIVE_ACCEPTED"));return;}
        Pass(TEXT("CORRUPT_PACKED_ARCHIVE_REJECTED"));
        int64 Total=0,Issued=0;
        if(!City->AuditConservation(Total,Issued)||Total!=Issued||Issued!=Run->IssuedMoney)
        {Fail(TEXT("TRANSACTIONS_CREATED_OR_DESTROYED_MONEY"));return;}
        Pass(TEXT("ALL_CITY_MONEY_CONSERVED"));
        // Exercise the actual controller-removal order used by OpenLevel: Logout
        // captures live cargo, then the city's teardown save must retain it.
        auto GM=World->GetAuthGameMode<AVoyagerGameMode>();
        const int32 ExpectedMinerals=PS->Minerals,ExpectedUpgrades=PS->Upgrades,ExpectedKills=PS->PirateKills;
        const TArray<int64> ExpectedVisited=PS->Visited;
        if(!GM||!PC->Destroy()){Fail(TEXT("CONTROLLER_TEARDOWN_FIXTURE"));return;}
        if(!GM->SaveExpedition(true,City)){Fail(TEXT("CONTROLLER_TEARDOWN_SAVE_FAILED"));return;}
        auto TeardownSave=Cast<UVoyagerSave>(UGameplayStatics::LoadGameFromSlot(TEXT("Voyager-Automation-CityLife"),0));
        if(!TeardownSave||TeardownSave->Minerals!=ExpectedMinerals||TeardownSave->Upgrades!=ExpectedUpgrades||
            TeardownSave->PirateKills!=ExpectedKills||TeardownSave->Visited!=ExpectedVisited||TeardownSave->CityArchives.Num()!=15)
        {Fail(TEXT("CONTROLLER_TEARDOWN_LOST_CARGO"));return;}
        Pass(TEXT("CONTROLLER_TEARDOWN_PRESERVES_CARGO"));
        UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY LIFE AUDIT METRICS cities=%d residents=%d archive_bytes=%lld peak_worker_ms=%.3f avg_fps=%.2f worst_frame_ms=%.3f frames=%llu total_seconds=%.2f"),Keys.Num(),V.SystemPopulation,Bytes,Run->PeakTickMs,Run->FrameSeconds>0?double(Run->FrameCount)/Run->FrameSeconds:0.0,Run->WorstFrameMs,Run->FrameCount,Now-Run->Started);
        Run->bFinished=true;UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY LIFE AUDIT COMPLETE PASS checks=%d"),Run->Checks);FPlatformMisc::RequestExit(false);return;
    }
    }
}
struct FRegistration
{
    FDelegateHandle Handle;
    FRegistration(){Handle=FWorldDelegates::OnWorldPostActorTick.AddStatic(&Tick);}
    ~FRegistration(){FWorldDelegates::OnWorldPostActorTick.Remove(Handle);}
} Registration;
}
