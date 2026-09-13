#include "VoyagerCharacter.h"
#include "VoyagerHUD.h"
#include "VoyagerItems.h"
#include "VoyagerGameMode.h"
#include "VoyagerCityLife.h"
#include "VoyagerSettlement.h"
#include "VoyagerDestruction.h"
#include "VoyagerShip.h"
#include "VoyagerLaw.h"
#include "VoyagerData.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"

namespace
{
    bool AtCookingStation(AVoyagerController* PC,AVoyagerCharacter* Explorer,AVoyagerState* State)
    {
        if(auto* GM=PC->GetWorld()->GetAuthGameMode<AVoyagerGameMode>())
            if(auto* Ship=GM->ShipFor(PC))
                if(Ship->bLanded&&FVector::DistSquared(Explorer->GetActorLocation(),Ship->GetActorLocation())<=FMath::Square(1200.0))return true;
        const int32 Planet=Voyager::NearestPlanet(State->SystemSeed,Explorer->GetActorLocation());
        if(Voyager::SurfaceAltitude(State->SystemSeed,Planet,Explorer->GetActorLocation())>35000)return false;
        for(int32 Site=0;Site<AVoyagerSettlement::SitesPerPlanet;++Site)
        {
            const int32 Index=AVoyagerSettlement::FindBuildingAt(State->SystemSeed,Planet,Site,Explorer->GetActorLocation());
            FVoyagerBuildingInfo Info;
            if(Index==INDEX_NONE||!AVoyagerSettlement::GetBuildingInfo(State->SystemSeed,Planet,Site,Index,Info))continue;
            if(Info.Role!=0&&Info.Role!=4)continue;
            if(auto* Destruction=AVoyagerDestruction::Find(PC->GetWorld()))
                if(AVoyagerDestruction::SurvivingFloors(Destruction->FindDamage(State->SystemSeed,Planet,Site,Index),Info.FloorCount)<=0)continue;
            return true;
        }
        return false;
    }
}

void AVoyagerController::ToggleBackpack()
{
    if(bBackpackVisible){CloseCityPhone();return;}
    if(!IsLocalController())return;
    if(bCityPhoneVisible){bBackpackVisible=true;return;}
    ToggleCityPhone();
    if(bCityPhoneVisible)bBackpackVisible=true;
}

void AVoyagerController::ServerSurvivalAction_Implementation(uint8 Action)
{
    if(!HasAuthority()||Action<1||Action>8)return;
    const float Now=GetWorld()->GetTimeSeconds();
    if(Now-LastSurvivalAction<.3f)return;
    LastSurvivalAction=Now;
    auto* PS=GetPlayerState<AVoyagerPlayerState>();auto* State=GetWorld()->GetGameState<AVoyagerState>();
    auto* Explorer=Cast<AVoyagerCharacter>(GetPawn());
    if(!PS||!State||State->bTransitioning||!Explorer||Explorer->Health<=0.f)
    {Notify(TEXT("Use survival equipment while on foot and alive."));return;}
    if(auto* Law=AVoyagerLaw::Find(GetWorld()))if(Law->IsJailed(this))
    {Notify(TEXT("Your equipment is secured while you are in custody."));return;}

    // Validate the entire recipe in a private copy before changing the player's
    // replicated state. No recipe can leave partial ingredient deductions.
    TArray<int32> Next=PS->Items;Next.SetNum(int32(EVoyagerItem::Count));
    for(int32 Count:Next)if(Count<0||Count>999){Notify(TEXT("Inventory is unavailable."));return;}
    int32 Minerals=PS->Minerals;
    auto Consume=[&](EVoyagerItem Item,int32 Amount)
    {
        int32& Count=Next[int32(Item)];if(Count<Amount)return false;Count-=Amount;return true;
    };
    auto Produce=[&](EVoyagerItem Item,int32 Amount)
    {
        int32& Count=Next[int32(Item)];if(Count>999-Amount)return false;Count+=Amount;return true;
    };
    auto Spend=[&](int32 Amount){if(Minerals<Amount)return false;Minerals-=Amount;return true;};
    auto Missing=[&](){Notify(TEXT("Missing ingredients or the output stack is full. The recipe is listed in your backpack."));};
    int32 Food=0,Water=0,Healing=0;float RawDamage=0.f;
    FString Message;FHitResult ChargeHit;bool bPlaceCharge=false;
    switch(Action)
    {
    case 1:
        if(!Consume(EVoyagerItem::RawMeat,1)){Missing();return;}
        Food=200;RawDamage=8.f;Message=TEXT("Ate raw meat: nourishment restored, but unsafe food costs 8 health. Cook your next meal.");break;
    case 2:
        if(!AtCookingStation(this,Explorer,State)){Notify(TEXT("Cook beside your landed ship (within 12 m), or inside a cafe or residence."));return;}
        if(!Consume(EVoyagerItem::RawMeat,1)||!Spend(1)||!Produce(EVoyagerItem::CookedMeat,1)){Missing();return;}
        Message=TEXT("Cooked one meal using raw meat and 1 mineral for the heater.");break;
    case 3:
        if(!Consume(EVoyagerItem::CookedMeat,1)){Missing();return;}
        Food=500;Water=80;Healing=80;Message=TEXT("Ate a cooked meal: nourishment, hydration and health restored.");break;
    case 4:
        if(!Consume(EVoyagerItem::Hide,2)||!Consume(EVoyagerItem::Bone,2)||!Spend(5)||!Produce(EVoyagerItem::Medkit,1)){Missing();return;}
        Message=TEXT("Crafted a medkit from 2 hide, 2 bone and 5 minerals.");break;
    case 5:
        if(Explorer->Health>=99.9f){Notify(TEXT("You are already at full health. Your medkit is preserved."));return;}
        if(!Consume(EVoyagerItem::Medkit,1)){Missing();return;}
        Healing=350;Message=TEXT("Used a medkit: restores up to 35 health.");break;
    case 6:
        if(!Consume(EVoyagerItem::Bone,2)||!Spend(12)||!Produce(EVoyagerItem::DemolitionCharge,1)){Missing();return;}
        Message=TEXT("Crafted a demolition charge from 2 bone and 12 minerals.");break;
    case 7:
    {
        if(!Consume(EVoyagerItem::DemolitionCharge,1)){Missing();return;}
        // The origin is the authoritative pawn eye, never a client supplied hit.
        const FVector Eye=Explorer->GetPawnViewLocation();
        FCollisionQueryParams Params(SCENE_QUERY_STAT(VoyagerChargePlacement),true,Explorer);
        if(!GetWorld()->LineTraceSingleByChannel(ChargeHit,Eye,Eye+GetControlRotation().Vector()*3000.f,ECC_Visibility,Params))
        {Notify(TEXT("Aim at a building wall, floor or window within 30 m to attach the charge."));return;}
        auto* Settlement=Cast<AVoyagerSettlement>(ChargeHit.GetActor());int32 System=0,Planet=0,Site=0,Building=0;
        if(!Settlement||!Settlement->ResolveBuildingHit(ChargeHit,System,Planet,Site,Building)||System!=State->SystemSeed)
        {Notify(TEXT("A demolition charge must attach to an actual building surface."));return;}
        if(!AVoyagerDestruction::Find(GetWorld())){Notify(TEXT("Building demolition is unavailable."));return;}
        bPlaceCharge=true;Message=TEXT("Demolition charge armed. Three-second fuse: move at least 22 m away!");break;
    }
    case 8:
        if(!Spend(8)||!Produce(EVoyagerItem::EnergyCell,30)){Missing();return;}
        Message=TEXT("Crafted 30 energy cells using 8 minerals.");break;
    default:return;
    }

    if(Food>0||Water>0||Healing>0)
    {
        auto* Life=AVoyagerCityLife::Find(GetWorld());
        if(!Life||!Life->ApplyFieldCare(this,Food,Water,Healing))
        {Notify(TEXT("Survival services are busy. Your item was preserved; try again shortly."));return;}
    }
    if(bPlaceCharge)
    {
        FActorSpawnParameters Params;Params.Owner=this;Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        const FVector At=ChargeHit.ImpactPoint+ChargeHit.ImpactNormal*12.f;
        auto* Charge=GetWorld()->SpawnActor<AVoyagerDemolitionCharge>(At,FRotationMatrix::MakeFromZ(ChargeHit.ImpactNormal).Rotator(),Params);
        if(!Charge){Notify(TEXT("Unable to place the charge. Your charge was preserved."));return;}
        Charge->Arm(this);
    }
    PS->Items=MoveTemp(Next);PS->Minerals=Minerals;PS->ForceNetUpdate();
    if(RawDamage>0.f)UGameplayStatics::ApplyDamage(Explorer,RawDamage,nullptr,Explorer,nullptr);
    if(auto* GM=GetWorld()->GetAuthGameMode<AVoyagerGameMode>())GM->SaveExpedition();
    Notify(Message);
    UE_LOG(LogTemp,Display,TEXT("VOYAGER SURVIVAL ACTION player=%s action=%d minerals=%d raw=%d cooked=%d medkit=%d charges=%d cells=%d"),
        *PS->GetName(),Action,Minerals,PS->ItemCount(EVoyagerItem::RawMeat),PS->ItemCount(EVoyagerItem::CookedMeat),
        PS->ItemCount(EVoyagerItem::Medkit),PS->ItemCount(EVoyagerItem::DemolitionCharge),PS->ItemCount(EVoyagerItem::EnergyCell));
}

void AVoyagerHUD::DrawBackpack(AVoyagerController* PC)
{
    if(!PC||!Canvas||!GEngine)return;
    const auto* PS=PC->GetPlayerState<AVoyagerPlayerState>();
    const float Scale=FMath::Max(.1f,FMath::Min(Canvas->SizeX/1600.f,Canvas->SizeY/900.f));
    const float W=Canvas->SizeX/Scale,H=Canvas->SizeY/Scale,Width=1180,Height=760,Left=(W-Width)*.5f,Top=(H-Height)*.5f;
    const FLinearColor White(.91f,.96f,.97f),Muted(.51f,.66f,.71f),Mint(.4f,.96f,.82f),Amber(1,.73f,.39f);
    const FLinearColor Card(.018f,.041f,.057f,.98f),Line(.15f,.29f,.34f);
    auto Rect=[&](float X,float Y,float BW,float BH,FLinearColor C){DrawRect(C,X*Scale,Y*Scale,BW*Scale,BH*Scale);};
    auto Text=[&](const FString& Value,float X,float Y,float Size,FLinearColor C,float Limit=0.f)
    {
        float TW=0,TH=0,Actual=Size;GetTextSize(Value,TW,TH,GEngine->GetMediumFont(),Size*Scale*1.8f);
        if(Limit>0&&TW>Limit*Scale)Actual*=Limit*Scale/TW;
        DrawText(Value,C,X*Scale,Y*Scale,GEngine->GetMediumFont(),Actual*Scale*1.8f,false);
    };
    Rect(0,0,W,H,FLinearColor(.002f,.007f,.014f,.69f));Rect(Left,Top,Width,Height,FLinearColor(.006f,.018f,.028f,.985f));
    Rect(Left,Top,Width,2,Mint);Rect(Left,Top,3,78,Mint);
    Text(TEXT("R I F T B O U N D   /   F I E L D   E Q U I P M E N T"),Left+30,Top+23,.79f,White);
    Text(TEXT("BACKPACK & CRAFTING"),Left+30,Top+61,1.0f,Mint);
    Text(TEXT("EXPEDITION MINERALS"),Left+870,Top+20,.49f,Muted);
    Text(PS?FString::FromInt(PS->Minerals):TEXT("..."),Left+870,Top+43,1.32f,White);
    Text(TEXT("Hunt wildlife with your sidearm. Approach the carcass and press E to harvest."),Left+30,Top+113,.62f,Muted,1115);
    Rect(Left+30,Top+152,1120,1,Line);
    Text(TEXT("YOUR SUPPLIES"),Left+30,Top+174,.62f,Muted);
    Text(TEXT("RECIPES & ACTIONS  /  PRESS 1-8"),Left+430,Top+174,.62f,Muted);
    for(int32 Index=0;Index<int32(EVoyagerItem::Count);++Index)
    {
        const float Y=Top+212+Index*47;const int32 Count=PS?PS->ItemCount(EVoyagerItem(Index)):0;
        Rect(Left+30,Y-7,360,41,Card);Text(VoyagerItems::Name(EVoyagerItem(Index)),Left+43,Y,.68f,White,270);
        Text(FString::FromInt(Count),Left+337,Y,.71f,Count>0?Mint:Muted,46);
    }
    static const TCHAR* Names[]={TEXT("1  Eat raw meat"),TEXT("2  Cook a meal"),TEXT("3  Eat cooked meat"),TEXT("4  Craft a medkit"),
        TEXT("5  Use a medkit"),TEXT("6  Craft a demolition charge"),TEXT("7  Attach demolition charge"),TEXT("8  Craft 30 energy cells")};
    static const TCHAR* Details[]={TEXT("1 raw meat  /  +20 nourishment, -8 health"),TEXT("1 raw meat + 1 mineral  /  ship, cafe or residence"),
        TEXT("1 cooked meat  /  +50 nourishment, +8 hydration and health"),TEXT("2 hide + 2 bone + 5 minerals"),TEXT("1 medkit  /  restores up to 35 health"),
        TEXT("2 bone + 12 minerals"),TEXT("Aim at a building within 30 m  /  3-second fuse, 22 m blast"),TEXT("8 minerals  /  ammunition for your sidearm")};
    for(int32 Index=0;Index<8;++Index)
    {
        const float Y=Top+208+Index*55;
        Rect(Left+430,Y,720,50,Card);Text(Names[Index],Left+443,Y+5,.69f,Index==0||Index==6?Amber:Mint,686);
        Text(Details[Index],Left+443,Y+29,.50f,Muted,686);
    }
    Text(TEXT("COOKING STATION"),Left+30,Top+560,.52f,Muted);
    Text(TEXT("Stand within 12 m of your landed ship,"),Left+30,Top+587,.55f,White,359);
    Text(TEXT("or enter a cafe or residence."),Left+30,Top+612,.55f,White,359);
    Rect(Left+30,Top+683,1120,1,Line);
    if(PC->NoticeTime>0&&!PC->Notice.IsEmpty())Text(PC->Notice,Left+30,Top+699,.59f,Amber,1120);
    else Text(TEXT("The world keeps running. Find cover before opening your backpack or using equipment."),Left+30,Top+699,.55f,Muted,1120);
    Text(TEXT("1-8  SELECT ACTION     I / ESC / BACKSPACE  CLOSE     P  CITY PHONE"),Left+30,Top+731,.56f,Mint,1120);
}
