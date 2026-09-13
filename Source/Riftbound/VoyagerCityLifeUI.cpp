#include "VoyagerCharacter.h"
#include "VoyagerHUD.h"
#include "VoyagerCityLife.h"
#include "VoyagerGameMode.h"
#include "VoyagerSettlement.h"
#include "VoyagerData.h"
#include "Components/InputComponent.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

namespace
{
FString CityCredits(int64 Minor)
{
    const int64 Absolute=Minor<0?-Minor:Minor;
    return FString::Printf(TEXT("%s%lld.%02lld"),Minor<0?TEXT("-"):TEXT(""),Absolute/100,Absolute%100);
}
const TCHAR* CityGuideName(int32 Target)
{
    static const TCHAR* Names[]={TEXT("OFF"),TEXT("HOME"),TEXT("WORK"),TEXT("MARKET"),TEXT("CIVIC SECURITY")};
    return Names[FMath::Clamp(Target,0,4)];
}
}

void AVoyagerController::ToggleCityPhone()
{
    if(bBackpackVisible&&bCityPhoneVisible){bBackpackVisible=false;return;}
    if(bCityPhoneVisible){CloseCityPhone();return;}
    if(!IsLocalController()||bMenuVisible||!GetPawn())return;
    CloseConversation();
    CityPhonePawn=GetPawn();
    // Release held tools, jumping and boost before removing the pawn's input.
    // The controller keeps receiving the phone keys while both pawn types are gated.
    if(UInputComponent* PawnInput=GetPawn()->InputComponent)
    {
        for(int32 Index=0;Index<PawnInput->GetNumActionBindings();++Index)
        {
            const auto& Binding=PawnInput->GetActionBinding(Index);
            if(Binding.KeyEvent==IE_Released)Binding.ActionDelegate.Execute(EKeys::Invalid);
        }
        for(auto& Axis:PawnInput->AxisBindings)Axis.AxisDelegate.Execute(0.f);
    }
    FlushPressedKeys();
    bCityPhoneVisible=true;bMenuVisible=true;
    GetPawn()->DisableInput(this);
    SetIgnoreMoveInput(true);SetIgnoreLookInput(true);
}

void AVoyagerController::CloseCityPhone()
{
    bBackpackVisible=false;
    if(!bCityPhoneVisible)return;
    if(CityPhonePawn.IsValid()&&CityPhonePawn.Get()==GetPawn())CityPhonePawn->EnableInput(this);
    CityPhonePawn.Reset();bCityPhoneVisible=false;bMenuVisible=false;
    SetIgnoreMoveInput(false);SetIgnoreLookInput(false);
    FlushPressedKeys();
}

void AVoyagerController::CityPhonePreviousPage(){if(bCityPhoneVisible&&!bBackpackVisible)CityPhonePage=(CityPhonePage+4)%5;}
void AVoyagerController::CityPhoneNextPage(){if(bCityPhoneVisible&&!bBackpackVisible)CityPhonePage=(CityPhonePage+1)%5;}
void AVoyagerController::CityPhoneFour(){CityPhoneAction(4);}
void AVoyagerController::CityPhoneFive(){CityPhoneAction(5);}
void AVoyagerController::CityPhoneSix(){CityPhoneAction(6);}
void AVoyagerController::CityPhoneSeven(){CityPhoneAction(7);}
void AVoyagerController::CityPhoneEight(){CityPhoneAction(8);}
void AVoyagerController::CycleCityGuide()
{
    if(bMenuVisible&&!bCityPhoneVisible)return;
    CityGuideTarget=(CityGuideTarget+1)%5;
    Notice=FString::Printf(TEXT("City guide: %s. N cycles home, work, market, Civic Security and off."),CityGuideName(CityGuideTarget));NoticeTime=8.f;
}
void AVoyagerController::ClientReceiveCityLife_Implementation(const FVoyagerCityLifeView& View){CityLifeView=View;}
void AVoyagerController::ServerCityAction_Implementation(uint8 Action,int32 Argument)
{
    if(auto* City=AVoyagerCityLife::Find(GetWorld()))City->HandleAction(this,Action,Argument);
}
void AVoyagerController::CityPhoneAction(int32 Number)
{
    if(!bCityPhoneVisible||Number<1||Number>8)return;
    if(bBackpackVisible){ServerSurvivalAction(uint8(Number));return;}
    if(!CityLifeView.bAvailable)return;
    auto Send=[this](EVoyagerCityAction Action,int32 Argument=0){ServerCityAction(static_cast<uint8>(Action),Argument);};
    switch(CityPhonePage)
    {
    case 0:
        switch(Number)
        {
        case 1:Send(EVoyagerCityAction::ConsumeGood,0);break;
        case 2:Send(EVoyagerCityAction::ConsumeGood,1);break;
        case 3:Send(EVoyagerCityAction::ConsumeGood,7);break;
        case 4:Send(EVoyagerCityAction::Rest);break;
        case 5:Send(EVoyagerCityAction::Wash);break;
        case 6:Send(EVoyagerCityAction::ConsumeGood,5);break;
        case 7:Send(EVoyagerCityAction::ConsumeGood,6);break;
        case 8:Send(EVoyagerCityAction::Recycle);break;
        }
        break;
    case 1:Send(EVoyagerCityAction::BuyGood,Number-1);break;
    case 2:
        switch(Number)
        {
        case 1:Send(EVoyagerCityAction::ApplyJob);break;
        case 2:Send(EVoyagerCityAction::StartWork);break;
        case 3:Send(EVoyagerCityAction::StopWork);break;
        case 4:Send(EVoyagerCityAction::RentHome);break;
        case 5:Send(EVoyagerCityAction::PayBills);break;
        case 6:Send(EVoyagerCityAction::SellMinerals,10);break;
        case 7:Send(EVoyagerCityAction::Recycle);break;
        case 8:CycleCityGuide();break;
        }
        break;
    case 3:if(Number==1)Send(EVoyagerCityAction::PayFine);break;
    default:break;
    }
}

void AVoyagerHUD::DrawCityPhone(AVoyagerController* PC)
{
    if(!PC||!Canvas||!GEngine)return;
    if(PC->bBackpackVisible){DrawBackpack(PC);return;}
    const float Scale=FMath::Max(.1f,FMath::Min(Canvas->SizeX/1600.f,Canvas->SizeY/900.f));
    const float ViewWidth=Canvas->SizeX/Scale,ViewHeight=Canvas->SizeY/Scale;
    const float Width=1180,Height=760,Left=(ViewWidth-Width)*.5f,Top=(ViewHeight-Height)*.5f;
    const FLinearColor White(.91f,.96f,.97f),Muted(.51f,.66f,.71f),Mint(.40f,.96f,.82f);
    const FLinearColor Amber(1.f,.73f,.39f),Red(1.f,.38f,.30f),Line(.15f,.29f,.34f);
    const FLinearColor Card(.018f,.041f,.057f,.98f),Track(.09f,.16f,.20f);
    auto Rect=[&](float X,float Y,float W,float H,FLinearColor Color){DrawRect(Color,X*Scale,Y*Scale,W*Scale,H*Scale);};
    auto Text=[&](const FString& Value,float X,float Y,float Size,FLinearColor Color,float MaxWidth=0.f)
    {
        float Actual=Size,TW=0,TH=0;
        GetTextSize(Value,TW,TH,GEngine->GetMediumFont(),Size*Scale*1.8f);
        if(MaxWidth>0&&TW>MaxWidth*Scale)Actual*=MaxWidth*Scale/TW;
        DrawText(Value,Color,X*Scale,Y*Scale,GEngine->GetMediumFont(),Actual*Scale*1.8f,false);
    };
    auto Wrap=[&](const FString& Value,float X,float Y,float MaxWidth,int32 MaxLines,FLinearColor Color,float Size=.67f)
    {
        TArray<FString> Words;Value.ParseIntoArrayWS(Words);
        FString LineText;int32 Count=0;
        for(const FString& Word:Words)
        {
            const FString Candidate=LineText.IsEmpty()?Word:LineText+TEXT(" ")+Word;
            float TW=0,TH=0;GetTextSize(Candidate,TW,TH,GEngine->GetMediumFont(),Size*Scale*1.8f);
            if(TW>MaxWidth*Scale&&!LineText.IsEmpty())
            {
                if(Count>=MaxLines-1){Text(LineText+TEXT("..."),X,Y,Size,Color,MaxWidth);return;}
                Text(LineText,X,Y,Size,Color,MaxWidth);Y+=25;LineText=Word;++Count;
            }
            else LineText=Candidate;
        }
        if(!LineText.IsEmpty())Text(LineText,X,Y,Size,Color,MaxWidth);
    };
    auto Rule=[&](float Y){Rect(Left+28,Y,Width-56,1,Line);};
    const auto& V=PC->CityLifeView;
    Rect(0,0,ViewWidth,ViewHeight,FLinearColor(.002f,.007f,.014f,.69f));
    Rect(Left,Top,Width,Height,FLinearColor(.006f,.018f,.028f,.985f));
    Rect(Left,Top,Width,2,Mint);Rect(Left,Top,3,78,Mint);
    Text(TEXT("R I F T B O U N D   /   C I T Y   L I F E"),Left+30,Top+20,.79f,White);
    Text(V.bAvailable?V.CityName:TEXT("Connecting to city services..."),Left+30,Top+57,.78f,Mint,650);
    Text(TEXT("AVAILABLE BALANCE"),Left+870,Top+15,.49f,Muted);
    Text(CityCredits(V.CreditsMinor)+TEXT("  CREDITS"),Left+870,Top+36,1.02f,White,280);
    Text(TEXT("City money / separate from expedition minerals"),Left+870,Top+69,.43f,Muted,280);
    const FString Location=V.bOnFoot&&V.bAtCity?(V.CurrentBuildingName.IsEmpty()?TEXT("City streets"):V.CurrentBuildingName):TEXT("Remote access / land to use city services");
    Text(Location,Left+30,Top+95,.55f,Muted,810);
    Text(V.ClockText,Left+1010,Top+95,.56f,Mint,140);
    static const TCHAR* Pages[]={TEXT("OVERVIEW"),TEXT("MARKET"),TEXT("WORK & HOME"),TEXT("JUSTICE"),TEXT("NEWS")};
    for(int32 Page=0;Page<5;++Page)
    {
        const float X=Left+28+Page*226;
        if(Page==PC->CityPhonePage){Rect(X,Top+123,216,39,Card);Rect(X,Top+160,216,2,Mint);}
        Text(Pages[Page],X+14,Top+133,.63f,Page==PC->CityPhonePage?Mint:Muted,190);
    }
    const float X=Left+30,Y=Top+185;
    if(!V.bAvailable)
    {
        Text(TEXT("Your city account is loading"),X,Y+30,.96f,White);
        Wrap(TEXT("The city keeps its own residents, shops and records. Your phone will update when its current state arrives."),X,Y+84,740,3,Muted);
    }
    else if(PC->CityPhonePage==0)
    {
        Text(TEXT("YOUR DAILY LIFE"),X,Y,.67f,Muted);
        Text(V.bWorking?TEXT("On shift"):V.bEmployed?TEXT("Employed"):TEXT("Seeking work"),X,Y+30,1.05f,White);
        Wrap(V.ShiftStatus,X,Y+72,660,2,Muted);
        Text(TEXT("HOME"),X,Y+132,.50f,Muted);
        Text(V.HomeName,X,Y+153,.76f,White,625);
        Text(V.bRented?TEXT("Tenancy active"):TEXT("Visit your residence to arrange tenancy"),X,Y+183,.55f,V.bRented?Mint:Amber,625);
        Text(TEXT("WORKPLACE"),X,Y+220,.50f,Muted);
        Text(V.WorkName,X,Y+241,.76f,White,625);
        Text(TEXT("PERSONAL CARE  /  USE ITEMS FROM YOUR BAG"),X,Y+295,.53f,Muted);
        static const TCHAR* Care[]={TEXT("1  Eat food"),TEXT("2  Drink water"),TEXT("3  Take medicine"),TEXT("4  Rest at home"),TEXT("5  Wash at home"),TEXT("6  Use consumer goods"),TEXT("7  Use electronics"),TEXT("8  Recycle waste")};
        for(int32 I=0;I<8;++I)
        {
            const float AX=X+(I%2)*330,AY=Y+326+(I/2)*35;
            Rect(AX,AY-4,312,30,Card);Text(Care[I],AX+10,AY,.60f,Mint,288);
        }
        const float NX=Left+756;
        Text(TEXT("WELLBEING"),NX,Y,.67f,Muted);
        Text(TEXT("Higher is better"),NX,Y+27,.52f,Muted);
        static const TCHAR* Needs[]={TEXT("Nourishment"),TEXT("Hydration"),TEXT("Hygiene"),TEXT("Rest"),TEXT("Health"),TEXT("Energy"),TEXT("Social"),TEXT("Safety"),TEXT("Comfort")};
        for(int32 I=0;I<9;++I)
        {
            const float NY=Y+67+I*44;
            const float Value=V.Needs.IsValidIndex(I)?1.f-FMath::Clamp(V.Needs[I]/1000.f,0.f,1.f):1.f;
            const FLinearColor Color=Value<.25f?Red:Value<.5f?Amber:Mint;
            Text(Needs[I],NX,NY,.60f,White);
            Text(FString::Printf(TEXT("%d%%"),FMath::RoundToInt(Value*100)),NX+319,NY,.58f,Color);
            Rect(NX,NY+26,364,4,Track);Rect(NX,NY+26,364*Value,4,Color);
        }
    }
    else if(PC->CityPhonePage==1)
    {
        Text(TEXT("LOCAL MARKET"),X,Y,.77f,White);
        Text(TEXT("1-8  Buy one item at a shop / prices follow available stock"),X,Y+33,.60f,Muted,1110);
        Text(TEXT("ITEM"),X+14,Y+77,.51f,Muted);
        Text(TEXT("ON SHELF"),X+570,Y+77,.51f,Muted);
        Text(TEXT("PRICE / CREDITS"),X+760,Y+77,.51f,Muted);
        Text(TEXT("IN YOUR BAG"),X+978,Y+77,.51f,Muted);
        for(int32 I=0;I<FMath::Min(8,V.Goods.Num());++I)
        {
            const auto& Good=V.Goods[I];const float RowY=Y+105+I*43;
            Rect(X,RowY-6,1118,39,Card);
            Text(FString::Printf(TEXT("%d"),I+1),X+14,RowY,.66f,Mint);
            Text(Good.Name,X+53,RowY,.67f,White,480);
            Text(FString::FromInt(Good.Stock),X+580,RowY,.65f,Good.Stock>0?White:Amber);
            Text(CityCredits(Good.PriceMinor),X+762,RowY,.65f,V.CreditsMinor>=Good.PriceMinor?White:Amber);
            Text(FString::FromInt(Good.Owned),X+998,RowY,.65f,Mint);
        }
        Text(TEXT("Use food, water, medicine and household items from Overview. Materials support local production."),X,Y+467,.54f,Muted,1115);
    }
    else if(PC->CityPhonePage==2)
    {
        Text(TEXT("WORK & HOME"),X,Y,.77f,White);
        Text(TEXT("WORKPLACE"),X,Y+47,.50f,Muted);
        Text(V.WorkName,X,Y+70,.75f,White,655);
        Wrap(V.ShiftStatus,X,Y+106,655,2,Muted);
        Text(TEXT("HOME ADDRESS"),X,Y+177,.50f,Muted);
        Text(V.HomeName,X,Y+200,.75f,White,655);
        Text(V.bRented?TEXT("Tenancy active"):TEXT("Tenancy available at this residence"),X,Y+232,.56f,V.bRented?Mint:Amber,655);
        const float RX=Left+780;
        Text(TEXT("WAGE / PER SHIFT HOUR"),RX,Y+8,.50f,Muted);
        Text(CityCredits(V.WageMinor)+TEXT(" cr"),RX,Y+32,.96f,White);
        Text(TEXT("RENT"),RX,Y+88,.50f,Muted);
        Text(CityCredits(V.RentMinor)+TEXT(" cr"),RX,Y+111,.79f,White);
        Text(TEXT("OUTSTANDING BILLS"),RX,Y+161,.50f,Muted);
        Text(CityCredits(V.BillsMinor)+TEXT(" cr"),RX,Y+184,.79f,V.BillsMinor>0?Amber:Mint);
        Text(V.bUtilitiesConnected?TEXT("Utilities connected"):TEXT("Utilities disconnected"),RX,Y+226,.57f,V.bUtilitiesConnected?Mint:Red,330);
        Text(TEXT("SERVICES  /  VISIT THE RELEVANT BUILDING"),X,Y+286,.53f,Muted);
        const FString Actions[]={TEXT("1  Apply at this workplace"),TEXT("2  Begin your shift"),TEXT("3  End your shift"),TEXT("4  Rent your residence"),TEXT("5  Pay outstanding bills at home"),TEXT("6  Sell 10 expedition minerals"),TEXT("7  Recycle carried waste"),FString(TEXT("8  Navigation: "))+CityGuideName(PC->CityGuideTarget)+TEXT(" / cycle")};
        for(int32 I=0;I<8;++I)
        {
            const float AX=X+(I%2)*570,AY=Y+319+(I/2)*39;
            Rect(AX,AY-4,549,33,Card);Text(Actions[I],AX+10,AY,.61f,Mint,528);
        }
    }
    else if(PC->CityPhonePage==3)
    {
        Text(TEXT("CITY JUSTICE"),X,Y,.77f,White);
        Wrap(V.LegalStatus,X,Y+44,1060,2,V.FineMinor>0?Amber:Mint,.83f);
        Rect(X,Y+121,536,135,Card);Rect(X+566,Y+121,552,135,Card);
        Text(TEXT("OUTSTANDING FINE"),X+20,Y+138,.52f,Muted);
        Text(CityCredits(V.FineMinor)+TEXT(" CREDITS"),X+20,Y+174,1.25f,V.FineMinor>0?Amber:White,488);
        Text(TEXT("RECORDED OFFENSES"),X+586,Y+138,.52f,Muted);
        Text(FString::FromInt(V.CriminalRecord),X+586,Y+174,1.25f,White);
        Text(TEXT("1  PAY OUTSTANDING FINE"),X,Y+295,.77f,Mint);
        Wrap(TEXT("Payments transfer credits from your account into the city budget. Settling a fine does not erase your record."),X,Y+337,1030,3,Muted);
        Text(TEXT("CITY BUDGET"),X,Y+428,.50f,Muted);
        Text(CityCredits(V.GovernmentMinor)+TEXT(" credits"),X,Y+450,.67f,White);
    }
    else
    {
        Text(TEXT("CITY BULLETIN"),X,Y,.77f,White);
        Text(TEXT("Reports from the current city ledger"),X,Y+32,.57f,Muted);
        if(V.Bulletin.IsEmpty())Wrap(TEXT("No new reports. Market prices, employment and public finances continue to update."),X,Y+96,1040,3,Muted);
        for(int32 I=0;I<FMath::Min(5,V.Bulletin.Num());++I)
        {
            const float NY=Y+84+I*72;
            Rect(X,NY,1118,63,Card);Rect(X,NY,3,63,Mint);
            Wrap(V.Bulletin[I],X+19,NY+12,1072,2,White,.64f);
        }
        Text(FString::Printf(TEXT("%s residents in this city   /   %s in this star system"),*FString::FromInt(V.Population),*FString::FromInt(V.SystemPopulation)),X,Y+467,.55f,Muted,1100);
    }
    Rule(Top+683);
    if(PC->NoticeTime>0&&!PC->Notice.IsEmpty())Wrap(PC->Notice,Left+30,Top+694,1120,1,Amber,.58f);
    else Text(TEXT("Your world keeps running while the phone is open."),Left+30,Top+695,.54f,Muted);
    Text(TEXT("LEFT / RIGHT  CHANGE PAGE     1-8  SELECT ACTION     P / ESC / BACKSPACE  CLOSE"),Left+30,Top+731,.56f,Mint,1120);
}

void AVoyagerHUD::DrawCityGuide(AVoyagerController* PC)
{
    if(!PC||!Canvas||!GEngine||PC->CityGuideTarget==0||PC->bMenuVisible)return;
    auto* Pawn=Cast<AVoyagerCharacter>(PC->GetPawn());
    auto* State=GetWorld()->GetGameState<AVoyagerState>();
    const auto& V=PC->CityLifeView;
    if(!Pawn||!State||State->bTransitioning||!V.bAvailable||!V.bAtCity||V.CityKey<0||V.CityKey>=15)return;
    const FVector Position=Pawn->GetActorLocation();
    const float Scale=FMath::Max(.1f,FMath::Min(Canvas->SizeX/1600.f,Canvas->SizeY/900.f));
    const float W=Canvas->SizeX/Scale,H=Canvas->SizeY/Scale;
    const FLinearColor White(.91f,.97f,1.f),Muted(.49f,.65f,.71f),Mint(.37f,1.f,.79f),Amber(1.f,.70f,.30f);
    const FLinearColor Color=PC->CityGuideTarget==4?Amber:Mint;
    const double Now=GetWorld()->GetTimeSeconds();
    if(Now>=NextGuideSample||GuideSystem!=State->SystemSeed||GuideCity!=V.CityKey||GuideMode!=PC->CityGuideTarget)
    {
        NextGuideSample=Now+.5;GuideSystem=State->SystemSeed;GuideCity=V.CityKey;GuideMode=PC->CityGuideTarget;
        GuideBuilding=INDEX_NONE;GuideBuildingName.Empty();
        const int32 Requested=GuideMode==1?V.HomeBuilding:GuideMode==2?V.WorkplaceBuilding:INDEX_NONE;
        double Best=DBL_MAX;
        for(int32 Index=0;Index<AVoyagerSettlement::BuildingCount();++Index)
        {
            if(GuideMode<=2&&Index!=Requested)continue;
            FVoyagerBuildingInfo Info;
            if(!AVoyagerSettlement::GetBuildingInfo(GuideSystem,GuideCity/3,GuideCity%3,Index,Info))continue;
            if(GuideMode==3&&Info.Role!=2)continue;
            if(GuideMode==4&&Info.Role!=5)continue;
            const double Distance=FVector::DistSquared(Position,Info.DoorOutside);
            if(Distance>=Best)continue;
            Best=Distance;GuideBuilding=Index;GuideDoor=Info.DoorOutside;GuideUp=Info.Up;GuideBuildingName=Info.Name;
        }
    }
    auto Text=[&](const FString& Value,float X,float Y,float Size,FLinearColor C,float MaxWidth=0.f)
    {
        float TW=0,TH=0;GetTextSize(Value,TW,TH,GEngine->GetMediumFont(),Size*Scale*1.8f);
        if(MaxWidth>0&&TW>MaxWidth*Scale)Size*=MaxWidth*Scale/TW;
        DrawText(Value,C,X*Scale,Y*Scale,GEngine->GetMediumFont(),Size*Scale*1.8f,false);
    };
    auto Center=[&](const FString& Value,float X,float Y,float Size,FLinearColor C,float MaxWidth)
    {
        float TW=0,TH=0;GetTextSize(Value,TW,TH,GEngine->GetMediumFont(),Size*Scale*1.8f);
        if(TW>MaxWidth*Scale){Size*=MaxWidth*Scale/TW;TW=MaxWidth*Scale;}
        DrawText(Value,C,X*Scale-TW*.5f,Y*Scale,GEngine->GetMediumFont(),Size*Scale*1.8f,false);
    };
    auto Line=[&](float X1,float Y1,float X2,float Y2){DrawLine(X1*Scale,Y1*Scale,X2*Scale,Y2*Scale,Color,1.6f*Scale);};
    DrawRect(FLinearColor(.008f,.018f,.033f,.88f),16*Scale,296*Scale,420*Scale,86*Scale);
    DrawRect(Color,16*Scale,296*Scale,2*Scale,86*Scale);
    const bool bArrived=GuideBuilding!=INDEX_NONE&&V.CurrentBuilding==GuideBuilding;
    Text(FString(TEXT("CITY GUIDE / "))+CityGuideName(PC->CityGuideTarget)+(bArrived?TEXT(" / ARRIVED"):TEXT("")),32,308,.49f,Color,387);
    if(GuideBuilding==INDEX_NONE)
    {
        Text(TEXT("No assigned destination"),32,331,.65f,White,387);
        Text(TEXT("P phone to find work / N next destination"),32,359,.47f,Muted,387);return;
    }
    const double Meters=FVector::Dist(Position,GuideDoor)/100.0;
    Text(GuideBuildingName,32,331,.65f,White,387);
    Text(bArrived?TEXT("P phone for services / N next destination"):FString::Printf(TEXT("%.0f m to entrance / N next destination"),Meters),32,359,.47f,Muted,387);
    if(bArrived||PC->ConversationTime>0)return;
    FVector2D Projected;
    const bool bAhead=PC->ProjectWorldLocationToScreen(GuideDoor+GuideUp*230,Projected,true);
    const float XMin=585,XMax=W-470,YMin=390,YMax=H-310;
    float X=Projected.X/Scale,Y=Projected.Y/Scale;
    const bool bVisible=bAhead&&X>XMin&&X<XMax&&Y>YMin&&Y<YMax;
    float DX=0,DY=0;
    if(!bVisible)
    {
        const float CX=(XMin+XMax)*.5f,CY=(YMin+YMax)*.5f;
        if(bAhead){DX=X-CX;DY=Y-CY;}
        else
        {
            FVector View;FRotator Rotation;PC->GetPlayerViewPoint(View,Rotation);
            const FVector Direction=(GuideDoor-View).GetSafeNormal();
            DX=FVector::DotProduct(Direction,FRotationMatrix(Rotation).GetUnitAxis(EAxis::Y));
            DY=-FVector::DotProduct(Direction,FRotationMatrix(Rotation).GetUnitAxis(EAxis::Z));
        }
        if(FMath::Abs(DX)+FMath::Abs(DY)<.01f)DX=1;
        const float Factor=1.f/FMath::Max(FMath::Abs(DX)/((XMax-XMin)*.5f),FMath::Abs(DY)/((YMax-YMin)*.5f));
        X=CX+DX*Factor;Y=CY+DY*Factor;
        const FVector2D Direction=FVector2D(DX,DY).GetSafeNormal(),Side(-Direction.Y,Direction.X);
        const FVector2D Point(X,Y),A=Point+Direction*12,B=Point-Direction*8+Side*7,C=Point-Direction*8-Side*7;
        Line(A.X,A.Y,B.X,B.Y);Line(B.X,B.Y,C.X,C.Y);Line(C.X,C.Y,A.X,A.Y);
    }
    else{Line(X,Y-11,X+10,Y);Line(X+10,Y,X,Y+11);Line(X,Y+11,X-10,Y);Line(X-10,Y,X,Y-11);}
    Center(FString::Printf(TEXT("%s / %.0f m"),CityGuideName(PC->CityGuideTarget),Meters),X,Y+19,.57f,Color,276);
    Center(GuideBuildingName,X,Y+44,.47f,White,276);
}
