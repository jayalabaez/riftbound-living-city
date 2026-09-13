#include "VoyagerCharacter.h"
#include "VoyagerCityLife.h"
#include "VoyagerGameMode.h"
#include "VoyagerItems.h"
#include "VoyagerLaw.h"
#include "VoyagerShip.h"
#include "VoyagerData.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"
#include "Engine/DamageEvents.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

namespace VoyagerSurvivalAudit
{
    struct FRun
    {
        TWeakObjectPtr<UWorld> World;TWeakObjectPtr<AVoyagerController> PC;
        int32 Stage=0,Checks=0,BeforeMinerals=0;TArray<int32> BeforeItems;
        double Started=0,StageTime=0;float BeforeHealth=0;bool Finished=false;
        uint64 BeforeProcessed=0;
    };
    TUniquePtr<FRun> Run;
    void Pass(const TCHAR* Name){++Run->Checks;UE_LOG(LogTemp,Display,TEXT("VOYAGER SURVIVAL AUDIT PASS %s"),Name);}
    void Fail(const TCHAR* Name){Run->Finished=true;UE_LOG(LogTemp,Error,TEXT("VOYAGER SURVIVAL AUDIT FAIL stage=%d %s"),Run->Stage,Name);FPlatformMisc::RequestExit(false);}
    void Advance(double Now){++Run->Stage;Run->StageTime=Now;}
    void Before(AVoyagerPlayerState* PS){Run->BeforeItems=PS->Items;Run->BeforeMinerals=PS->Minerals;}
    bool Unchanged(AVoyagerPlayerState* PS){return PS->Items==Run->BeforeItems&&PS->Minerals==Run->BeforeMinerals;}
    bool HealthMatches(AVoyagerCityLife* Life,AVoyagerController* PC,AVoyagerCharacter* Pawn,int32 Expected,uint64 MinimumProcessed)
    {
        const auto View=Life->BuildView(PC);
        const int32 CoreHealth=View.Needs.IsValidIndex(4)?100-View.Needs[4]/10:INDEX_NONE;
        const bool Matches=View.bAvailable&&CoreHealth==Expected&&FMath::IsNearlyEqual(Pawn->Health,float(Expected))&&View.ProcessedInput>=MinimumProcessed;
        if(!Matches)UE_LOG(LogTemp,Error,TEXT("VOYAGER SURVIVAL HEALTH expected=%d pawn=%.2f core=%d processed=%llu minimum=%llu"),Expected,Pawn->Health,CoreHealth,View.ProcessedInput,MinimumProcessed);
        return Matches;
    }
    void Capture(const TCHAR* Name)
    {
        if(!FParse::Param(FCommandLine::Get(),TEXT("VoyagerSurvivalCapture")))return;
        FString Folder;FParse::Value(FCommandLine::Get(),TEXT("VoyagerSurvivalOutput="),Folder);
        if(Folder.IsEmpty())Folder=FPaths::ProjectSavedDir()/TEXT("VoyagerSurvivalAudit");
        IFileManager::Get().MakeDirectory(*Folder,true);FScreenshotRequest::RequestScreenshot(Folder/(FString(Name)+TEXT(".png")),true,false);
    }
    void Place(AVoyagerCharacter* Pawn,AVoyagerController* PC,FVector Position)
    {
        auto* State=Pawn->GetWorld()->GetGameState<AVoyagerState>();
        const int32 Planet=Voyager::NearestPlanet(State->SystemSeed,Position);const FVector Up=Voyager::SurfaceNormal(State->SystemSeed,Planet,Position);
        Pawn->GetCharacterMovement()->StopMovementImmediately();Pawn->GetCharacterMovement()->SetGravityDirection(-Up);
        Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
        Pawn->SetActorLocationAndRotation(Position,Voyager::TangentRotation(Up),false,nullptr,ETeleportType::TeleportPhysics);
        PC->SetControlRotation(Voyager::TangentRotation(Up));
    }
    void Tick(UWorld* World,ELevelTick Type,float D)
    {
        if(!FParse::Param(FCommandLine::Get(),TEXT("VoyagerSurvivalAudit"))||!World||!World->IsGameWorld()||World->GetNetMode()==NM_Client)return;
        if(!Run)
        {
            if(World->GetTimeSeconds()<8)return;
            auto* PC=Cast<AVoyagerController>(World->GetFirstPlayerController());if(!PC||!Cast<AVoyagerCharacter>(PC->GetPawn()))return;
            auto* Life=AVoyagerCityLife::Find(World);if(!Life||!Life->BuildView(PC).bAvailable)return;
            Run=MakeUnique<FRun>();Run->World=World;Run->PC=PC;Run->Started=Run->StageTime=FPlatformTime::Seconds();
        }
        if(Run->World.Get()!=World||Run->Finished)return;
        const double Now=FPlatformTime::Seconds(),Elapsed=Now-Run->StageTime;
        if(Now-Run->Started>70){Fail(TEXT("TIMEOUT"));return;}
        auto* PC=Run->PC.Get();auto* Pawn=PC?Cast<AVoyagerCharacter>(PC->GetPawn()):nullptr;
        auto* PS=PC?PC->GetPlayerState<AVoyagerPlayerState>():nullptr;auto* GM=World->GetAuthGameMode<AVoyagerGameMode>();auto* State=World->GetGameState<AVoyagerState>();
        auto* Life=AVoyagerCityLife::Find(World);
        if(!PC||!Pawn||!PS||!GM||!State||!Life)return;
        switch(Run->Stage)
        {
        case 0:
            PS->Items=VoyagerItems::StartingInventory();PS->Minerals=100;
            if(!PS->GrantHuntLoot(5,8,10)){Fail(TEXT("HUNT_LOOT_GRANT"));return;}
            Pass(TEXT("HUNT_LOOT_ITEMS"));PC->ToggleBackpack();
            if(!PC->bBackpackVisible||!PC->bCityPhoneVisible||!PC->IsMoveInputIgnored()||!PC->IsLookInputIgnored())
            {Fail(TEXT("BACKPACK_MODAL"));return;}
            Pass(TEXT("BACKPACK_MODAL_INPUT"));Capture(TEXT("Backpack"));Advance(Now);return;
        case 1:
        {
            if(Elapsed<2)return;PC->ToggleCityPhone();
            if(PC->bBackpackVisible||!PC->bCityPhoneVisible){Fail(TEXT("PHONE_SWITCH"));return;}
            PC->ToggleBackpack();PC->ToggleBackpack();
            if(PC->bBackpackVisible||PC->bCityPhoneVisible||PC->IsMoveInputIgnored()||PC->IsLookInputIgnored())
            {Fail(TEXT("BACKPACK_INPUT_RESTORE"));return;}
            Pass(TEXT("PHONE_SWITCH_AND_INPUT_RESTORE"));
            const int32 P=Voyager::NearestPlanet(State->SystemSeed,Pawn->GetActorLocation());
            Place(Pawn,PC,Voyager::SurfacePoint(State->SystemSeed,P,FVector(.03,.025,1).GetSafeNormal(),120));
            Before(PS);PC->ServerSurvivalAction(2);
            if(!Unchanged(PS)){Fail(TEXT("COOKING_WRONG_LOCATION"));return;}
            Pass(TEXT("COOKING_LOCATION_REJECTS_WITHOUT_LOSS"));Advance(Now);return;
        }
        case 2:
        {
            if(Elapsed<.8)return;auto* Ship=GM->ShipFor(PC);if(!Ship)return;
            Place(Pawn,PC,Ship->GetActorLocation()+Ship->GetActorRightVector()*600);
            Before(PS);PC->ServerSurvivalAction(2);
            if(PS->ItemCount(EVoyagerItem::RawMeat)!=4||PS->ItemCount(EVoyagerItem::CookedMeat)!=1||PS->Minerals!=99)
            {Fail(TEXT("COOK_RECIPE"));return;}
            Pass(TEXT("COOKS_BESIDE_OWN_SHIP"));Before(PS);PC->ServerSurvivalAction(2);
            if(!Unchanged(PS)){Fail(TEXT("RATE_LIMIT"));return;}
            Pass(TEXT("REPEATED_RPC_RATE_LIMIT"));Advance(Now);return;
        }
        case 3:
            if(Elapsed<.8)return;PC->ServerSurvivalAction(4);
            if(PS->ItemCount(EVoyagerItem::Medkit)!=1||PS->ItemCount(EVoyagerItem::Hide)!=6||PS->ItemCount(EVoyagerItem::Bone)!=8||PS->Minerals!=94)
            {Fail(TEXT("MEDKIT_RECIPE"));return;}
            Pass(TEXT("MEDKIT_RECIPE_EXACT_COST"));Advance(Now);return;
        case 4:
            if(Elapsed<.8)return;Run->BeforeHealth=Pawn->Health;PC->ServerSurvivalAction(1);
            if(PS->ItemCount(EVoyagerItem::RawMeat)!=3||Pawn->Health>Run->BeforeHealth-7.9f)
            {Fail(TEXT("RAW_FOOD_TRADEOFF"));return;}
            Pass(TEXT("RAW_FOOD_CONSUMED_WITH_HEALTH_COST"));Advance(Now);return;
        case 5:
            if(Elapsed<1.5)return;Run->BeforeHealth=Pawn->Health;PC->ServerSurvivalAction(5);
            if(PS->ItemCount(EVoyagerItem::Medkit)!=0){Fail(TEXT("MEDKIT_USE"));return;}
            Advance(Now);return;
        case 6:
            if(Elapsed<1.5)return;
            if(Pawn->Health<=Run->BeforeHealth+1.f){Fail(TEXT("FIELD_MEDICINE_HEALTH"));return;}
            Pass(TEXT("FIELD_MEDICINE_UPDATES_REAL_HEALTH"));PC->ServerSurvivalAction(3);
            if(PS->ItemCount(EVoyagerItem::CookedMeat)!=0){Fail(TEXT("COOKED_MEAL_USE"));return;}
            Pass(TEXT("COOKED_MEAL_CONSUMED"));Advance(Now);return;
        case 7:
            if(Elapsed<.8)return;PC->ServerSurvivalAction(8);
            if(PS->ItemCount(EVoyagerItem::EnergyCell)!=150||PS->Minerals!=86){Fail(TEXT("ENERGY_CELLS_RECIPE"));return;}
            Pass(TEXT("AMMUNITION_RECIPE_EXACT_COST"));Advance(Now);return;
        case 8:
            if(Elapsed<.8)return;PS->Items[int32(EVoyagerItem::EnergyCell)]=999;Before(PS);PC->ServerSurvivalAction(8);
            if(!Unchanged(PS)){Fail(TEXT("STACK_CAP_ATOMICITY"));return;}
            PS->Items[int32(EVoyagerItem::EnergyCell)]=150;Pass(TEXT("FULL_STACK_REJECTS_WITHOUT_LOSS"));Advance(Now);return;
        case 9:
            if(Elapsed<.8)return;PC->ServerSurvivalAction(6);
            if(PS->ItemCount(EVoyagerItem::DemolitionCharge)!=1||PS->ItemCount(EVoyagerItem::Bone)!=6||PS->Minerals!=74)
            {Fail(TEXT("CHARGE_RECIPE"));return;}
            Pass(TEXT("DEMOLITION_RECIPE_EXACT_COST"));Advance(Now);return;
        case 10:
            if(Elapsed<.8)return;PC->SetControlRotation(Pawn->GetActorUpVector().Rotation());Before(PS);PC->ServerSurvivalAction(7);
            if(!Unchanged(PS)){Fail(TEXT("EMPTY_SPACE_PLACEMENT"));return;}
            Pass(TEXT("INVALID_CHARGE_SURFACE_PRESERVES_ITEM"));PC->ToggleBackpack();Advance(Now);return;
        case 11:
            if(Elapsed<1.2)return;Capture(TEXT("CraftedSupplies"));
            if(!GM->SaveExpedition(true)){Fail(TEXT("SAVE"));return;}
            Pass(TEXT("INVENTORY_SAVE_COMPLETES"));Advance(Now);return;
        case 12:
            if(Elapsed<1)return;
            if(PC->bBackpackVisible)PC->ToggleBackpack();
            // Restore this fixture through the real worker, then let both its view
            // and the pawn cache reach 100 before exercising same-frame commands.
            if(!Life->ApplyFieldCare(PC,0,0,1000)){Fail(TEXT("HEALTH_FIXTURE_CARE"));return;}
            Advance(Now);return;
        case 13:
        {
            if(Elapsed<1.2)return;
            if(!HealthMatches(Life,PC,Pawn,100,0)){Fail(TEXT("HEALTH_FIXTURE_SYNCHRONIZED"));return;}
            if(!PS->AddItem(EVoyagerItem::Medkit,2)||!PS->AddItem(EVoyagerItem::CookedMeat,1)){Fail(TEXT("HEALTH_FIXTURE_ITEMS"));return;}
            Run->BeforeProcessed=Life->BuildView(PC).ProcessedInput;
            FDamageEvent Damage;Pawn->TakeDamage(34.f,Damage,nullptr,Pawn);PC->ServerSurvivalAction(5);
            if(!FMath::IsNearlyEqual(Pawn->Health,100.f)||PS->ItemCount(EVoyagerItem::Medkit)!=1)
            {Fail(TEXT("SAME_FRAME_MEDKIT_SUBMISSION"));return;}
            Advance(Now);return;
        }
        case 14:
        {
            if(Elapsed<1.2)return;
            if(!HealthMatches(Life,PC,Pawn,100,Run->BeforeProcessed+2)){Fail(TEXT("SAME_FRAME_MEDKIT_HEALTH"));return;}
            Pass(TEXT("SAME_FRAME_DAMAGE_MEDKIT_RESTORES_PAWN"));
            Run->BeforeProcessed=Life->BuildView(PC).ProcessedInput;
            FDamageEvent Damage;Pawn->TakeDamage(17.f,Damage,nullptr,Pawn);Advance(Now);return;
        }
        case 15:
            if(Elapsed<1.2)return;
            if(!HealthMatches(Life,PC,Pawn,83,Run->BeforeProcessed+1)){Fail(TEXT("HIT_AFTER_MEDKIT_HEALTH"));return;}
            Pass(TEXT("LATER_DAMAGE_AFTER_HEAL_STILL_APPLIES"));
            if(!Life->ApplyFieldCare(PC,0,0,1000)){Fail(TEXT("ORDERED_HEALTH_FIXTURE"));return;}
            Advance(Now);return;
        case 16:
        {
            if(Elapsed<1.2)return;
            if(!HealthMatches(Life,PC,Pawn,100,0)){Fail(TEXT("ORDERED_HEALTH_FIXTURE_SYNCHRONIZED"));return;}
            Run->BeforeProcessed=Life->BuildView(PC).ProcessedInput;
            FDamageEvent Damage;Pawn->TakeDamage(34.f,Damage,nullptr,Pawn);PC->ServerSurvivalAction(5);Pawn->TakeDamage(34.f,Damage,nullptr,Pawn);
            if(!FMath::IsNearlyEqual(Pawn->Health,66.f)||PS->ItemCount(EVoyagerItem::Medkit)!=0)
            {Fail(TEXT("ORDERED_MEDKIT_SUBMISSION"));return;}
            Advance(Now);return;
        }
        case 17:
        {
            if(Elapsed<1.2)return;
            // 100 - 34 + 35 (capped at 100) - 34 = 66. The second hit's
            // unsynchronized pawn value of 32 must not erase the earlier heal.
            if(!HealthMatches(Life,PC,Pawn,66,Run->BeforeProcessed+3)){Fail(TEXT("ORDERED_MEDKIT_DAMAGE_HEALTH"));return;}
            Pass(TEXT("SAME_FRAME_DAMAGE_MEDKIT_DAMAGE_PRESERVES_ORDER"));
            Run->BeforeProcessed=Life->BuildView(PC).ProcessedInput;
            FDamageEvent Damage;Pawn->TakeDamage(13.f,Damage,nullptr,Pawn);PC->ServerSurvivalAction(3);Pawn->TakeDamage(11.f,Damage,nullptr,Pawn);
            if(!FMath::IsNearlyEqual(Pawn->Health,50.f)||PS->ItemCount(EVoyagerItem::CookedMeat)!=0)
            {Fail(TEXT("ORDERED_MEAL_SUBMISSION"));return;}
            Advance(Now);return;
        }
        case 18:
            if(Elapsed<1.2)return;
            if(!HealthMatches(Life,PC,Pawn,50,Run->BeforeProcessed+3)){Fail(TEXT("ORDERED_MEAL_DAMAGE_HEALTH"));return;}
            Pass(TEXT("SAME_FRAME_DAMAGE_MEAL_DAMAGE_PRESERVES_ORDER"));
            if(!Life->ApplyFieldCare(PC,0,0,1000)){Fail(TEXT("LETHAL_BOUNDARY_FIXTURE"));return;}
            Advance(Now);return;
        case 19:
        {
            if(Elapsed<1.2)return;
            if(!HealthMatches(Life,PC,Pawn,100,0)||!PS->AddItem(EVoyagerItem::Medkit,3)){Fail(TEXT("LETHAL_BOUNDARY_FIXTURE_SYNCHRONIZED"));return;}
            Run->BeforeProcessed=Life->BuildView(PC).ProcessedInput;
            const FVector Position=Pawn->GetActorLocation();
            FDamageEvent Damage;Pawn->TakeDamage(34.f,Damage,nullptr,Pawn);PC->ServerSurvivalAction(5);Pawn->TakeDamage(80.f,Damage,nullptr,Pawn);
            if(!FMath::IsNearlyEqual(Pawn->Health,20.f)||FVector::DistSquared(Position,Pawn->GetActorLocation())>1.||PS->ItemCount(EVoyagerItem::Medkit)!=2)
            {Fail(TEXT("LETHAL_BOUNDARY_FALSE_RECOVERY"));return;}
            Advance(Now);return;
        }
        case 20:
        {
            if(Elapsed<1.2)return;
            if(!HealthMatches(Life,PC,Pawn,20,Run->BeforeProcessed+3)){Fail(TEXT("LETHAL_BOUNDARY_CORE_HEALTH"));return;}
            Pass(TEXT("HEAL_BEFORE_LETHAL_HIT_PREVENTS_FALSE_RECOVERY"));
            // A dead-pawn fixture must be rejected before inventory or core care is
            // submitted. Restore its presentation in this same tick afterward.
            Pawn->Health=0;Before(PS);PC->ServerSurvivalAction(5);
            if(Life->ApplyFieldCare(PC,200,80,350)||Pawn->Health!=0||!Unchanged(PS))
            {Fail(TEXT("DEAD_PAWN_CARE_RESURRECTED_OR_SPENT_ITEM"));return;}
            Pawn->Health=20;Pass(TEXT("DEAD_PLAYER_CARE_REJECTS_WITHOUT_ITEM_LOSS"));
            auto* Law=AVoyagerLaw::Find(World);if(!Law){Fail(TEXT("CUSTODY_FIXTURE_LAW"));return;}
            auto* Custody=NewObject<UVoyagerSave>();const auto View=Life->BuildView(PC);
            Custody->JailSeconds=3;Custody->JailSystem=State->SystemSeed;Custody->JailPlanet=View.CityKey/3;Custody->JailSite=View.CityKey%3;
            Law->RestoreCustody(PC,Custody);
            if(Law->IsJailed(PC)){Fail(TEXT("LEGACY_CUSTODY_STILL_ACTIVE"));return;}
            Advance(Now);return;
        }
        case 21:
        {
            if(Elapsed<.8)return;
            auto* Law=AVoyagerLaw::Find(World);if(!Law||Law->IsJailed(PC)){Fail(TEXT("LEGACY_CUSTODY_NOT_REMOVED"));return;}
            Before(PS);PC->ServerSurvivalAction(5);
            if(!FMath::IsNearlyEqual(Pawn->Health,55.f)||PS->ItemCount(EVoyagerItem::Medkit)!=1)
            {Fail(TEXT("LEGACY_CUSTODY_BLOCKS_CARE"));return;}
            Pass(TEXT("LEGACY_SENTENCE_DOES_NOT_BLOCK_CARE"));Advance(Now);return;
        }
        case 22:
            if(Elapsed<3)return;
            if(!HealthMatches(Life,PC,Pawn,55,0)){Fail(TEXT("CARE_HEALTH_NOT_PERSISTED"));return;}
            Run->Finished=true;UE_LOG(LogTemp,Display,TEXT("VOYAGER SURVIVAL AUDIT COMPLETE PASS checks=%d seconds=%.2f"),Run->Checks,Now-Run->Started);
            FPlatformMisc::RequestExit(false);return;
        default:Fail(TEXT("INVALID_STAGE"));return;
        }
    }
    struct FRegistration
    {
        FDelegateHandle Handle;
        FRegistration(){Handle=FWorldDelegates::OnWorldPostActorTick.AddStatic(&Tick);}
        ~FRegistration(){FWorldDelegates::OnWorldPostActorTick.Remove(Handle);}
    } Registration;
}
