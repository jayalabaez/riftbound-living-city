#include "VoyagerHUD.h"
#include "VoyagerLaw.h"
#include "VoyagerCharacter.h"
#include "VoyagerGameMode.h"
#include "VoyagerShip.h"
#include "VoyagerWorld.h"
#include "VoyagerData.h"
#include "VoyagerSettlement.h"
#include "VoyagerWildlife.h"
#include "VoyagerCitizen.h"
#include "Camera/CameraComponent.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"

void AVoyagerHUD::DrawHUD()
{
    Super::DrawHUD();
    if (!Canvas || !GEngine || !GetWorld()) return;
    auto* PC = Cast<AVoyagerController>(GetOwningPlayerController());
    auto* State = GetWorld()->GetGameState<AVoyagerState>();
    if (!PC || !State) return;
    APawn* Pawn = PC->GetPawn();
    auto* Explorer = Cast<AVoyagerCharacter>(Pawn);
    auto* Ship = Cast<AVoyagerShip>(Pawn);
    auto* Expedition = PC->GetPlayerState<AVoyagerPlayerState>();
    const FVector Position = Pawn ? Pawn->GetActorLocation() : FVector::ZeroVector;
    const int32 NearestPlanet = Voyager::NearestPlanet(State->SystemSeed, Position);
    const double AltitudeCm = FMath::Max(0.0, double(Ship ? Ship->SurfaceAltitude() : Voyager::SurfaceAltitude(State->SystemSeed, NearestPlanet, Position)));
    const double AltitudeKm = AltitudeCm / 100000.0;
    const double AtmosphereCm = Voyager::AtmosphereHeight;
    const bool bSpace = AltitudeCm >= AtmosphereCm;
    const bool bNearGround = AltitudeCm < 1000.0;
    const float AirDensity = Voyager::AtmosphereDensity(AltitudeCm);
    const TCHAR* Zone = bNearGround ? TEXT("SURFACE") : AltitudeCm < AtmosphereCm ? TEXT("ATMOSPHERE") : AltitudeKm < 90.0 ? TEXT("EXOSPHERE") : TEXT("ORBITAL SPACE");
    const double SampleTime = GetWorld()->GetTimeSeconds();
    const double SampleDelta = SampleTime - PreviousSampleTime;
    if (Pawn && TrackedPawn.Get() == Pawn && PreviousSystem == State->SystemSeed && SampleDelta > .00001 && !State->bTransitioning)
    {
        const FVector Velocity = Pawn->GetVelocity();
        DisplayedSpeedCm += (Velocity.Size() - DisplayedSpeedCm) * FMath::Clamp(SampleDelta * 7.0, 0.0, 1.0);
        RadialSpeedCm = FVector::DotProduct(Velocity, Voyager::SurfaceNormal(State->SystemSeed, NearestPlanet, Position));
    }
    else if(!Pawn || TrackedPawn.Get()!=Pawn || PreviousSystem!=State->SystemSeed || State->bTransitioning)
    { DisplayedSpeedCm = 0.0; RadialSpeedCm = 0.0; }
    TrackedPawn = Pawn; PreviousSampleTime = SampleTime; PreviousSystem = State->SystemSeed;
    const bool bReentry = AltitudeCm > 100000.0 && AltitudeCm < AtmosphereCm && RadialSpeedCm < -20000.0;
    const float W = Canvas->SizeX, H = Canvas->SizeY;
    const float S = FMath::Max(.1f, FMath::Min(W / 1600.f, H / 900.f));
    const float VW = W / S, VH = H / S;
    const FLinearColor White(.91f, .97f, 1.f), Muted(.49f, .65f, .71f), Mint(.37f, 1.f, .79f);
    const FLinearColor Amber(1.f, .70f, .30f), Red(1.f, .34f, .27f), Dark(.008f, .018f, .033f, .88f);
    const FLinearColor Line(.18f, .34f, .40f, .7f), Track(.12f, .20f, .25f, .85f);
    auto Rect = [&](float X, float Y, float Width, float Height, FLinearColor C)
    { DrawRect(C, X * S, Y * S, Width * S, Height * S); };
    auto Text = [&](const FString& T, float X, float Y, float Size, FLinearColor C)
    { DrawText(T, C, X * S, Y * S, GEngine->GetMediumFont(), Size * S * 1.8f, false); };
    auto Center = [&](const FString& T, float X, float Y, float Size, FLinearColor C, float MaxWidth = 0.f)
    {
        float TW = 0, TH = 0;
        GetTextSize(T, TW, TH, GEngine->GetMediumFont(), Size * S * 1.8f);
        const float Available = (MaxWidth > 0.f ? MaxWidth : VW - 80.f) * S;
        if (TW > Available) { Size *= Available / TW; TW = Available; }
        DrawText(T, C, X * S - TW * .5f, Y * S, GEngine->GetMediumFont(), Size * S * 1.8f, false);
    };
    auto Stroke = [&](float X1, float Y1, float X2, float Y2, FLinearColor C, float Thickness = 1.f)
    { DrawLine(X1 * S, Y1 * S, X2 * S, Y2 * S, C, Thickness * S); };
    auto Panel = [&](float X, float Y, float Width, float Height)
    { Rect(X, Y, Width, Height, Dark); Rect(X, Y, Width, 1, Line); };
    auto Bar = [&](float X, float Y, float Width, float Value, FLinearColor C)
    { Rect(X, Y, Width, 4, Track); Rect(X, Y, Width * FMath::Clamp(Value, 0.f, 1.f), 4, C); };

    if(SampleTime>=NextCitySample)
    {
        NextCitySample=SampleTime+.35;
        double CityDistance=DBL_MAX;
        for(int32 Site=0;Site<AVoyagerSettlement::SitesPerPlanet;++Site)
        {
            const FVector CenterPoint=Voyager::SurfacePoint(State->SystemSeed,NearestPlanet,AVoyagerSettlement::SiteDirection(State->SystemSeed,NearestPlanet,Site));
            const double Distance=FVector::DistSquared(Position,CenterPoint);
            if(Distance<CityDistance){CityDistance=Distance;NearbySite=Site;}
        }
        CityPopulation=0;InsideBuilding=INDEX_NONE;ClosestBuilding=INDEX_NONE;InteriorName.Empty();
        for(TActorIterator<AVoyagerCitizen> It(GetWorld());It;++It)
            if(It->IsAlive()&&It->PlanetIndex()==NearestPlanet&&It->SiteIndex()==NearbySite)++CityPopulation;
        if(Explorer&&CityDistance<FMath::Square(50000.0))
        {
            InsideBuilding=AVoyagerSettlement::FindBuildingAt(State->SystemSeed,NearestPlanet,NearbySite,Position);
            double DoorDistance=DBL_MAX;
            for(int32 Index=0;Index<AVoyagerSettlement::BuildingCount();++Index)
            {
                FVoyagerBuildingInfo Info;
                if(!AVoyagerSettlement::GetBuildingInfo(State->SystemSeed,NearestPlanet,NearbySite,Index,Info))continue;
                if(Index==InsideBuilding)InteriorName=Info.Name;
                const double Distance=FVector::DistSquared(Position,Info.DoorOutside);
                if(Distance<DoorDistance){DoorDistance=Distance;ClosestBuilding=Index;ClosestDoor=Info.DoorOutside;}
            }
        }
    }

    // Keep the center of the view clear: navigation and expedition data live on the edges.
    Panel(16,16,420,AltitudeKm<20.0?266:213);
    Rect(32, 30, 3, 61, Mint);
    Text(TEXT("R I F T B O U N D"), 49, 28, .73f, Muted);
    Text(TEXT("V O Y A G E R"), 47, 51, 1.28f, White);
    Text(Voyager::SystemName(State->SystemSeed) + TEXT("  /  PROCEDURAL FRONTIER"), 33, 108, .62f, Muted);
    Text(Voyager::PlanetName(State->SystemSeed, NearestPlanet).ToUpper(), 32, 139, 1.22f, White);
    Text(FString::Printf(TEXT("%s  /  %s"), Zone, Voyager::BiomeName(Voyager::Biome(State->SystemSeed, NearestPlanet))), 34, 177, .60f, Mint);
    Text(FString::Printf(TEXT("NEAREST WORLD  /  DIAMETER %.0f km"), Voyager::PlanetRadius(State->SystemSeed, NearestPlanet) * 2.0 / 100000.0), 34, 201, .49f, Muted);
    if(Pawn&&AltitudeKm<20.0)
    {
        const FVector SiteUp=AVoyagerSettlement::SiteDirection(State->SystemSeed,NearestPlanet,NearbySite);
        const FVector Site=Voyager::SurfacePoint(State->SystemSeed,NearestPlanet,SiteUp,4500.0);
        const double Distance=FVector::Dist(Position,Site)/100.0;
        Text(FString::Printf(TEXT("%s  /  %.1f km"),*AVoyagerSettlement::SiteName(State->SystemSeed,NearestPlanet,NearbySite),Distance/1000.0),34,228,.49f,Amber);
        FVector2D Projected;
        if(Distance<20000.0&&PC->ProjectWorldLocationToScreen(Site,Projected,true))
        {
            const float X=Projected.X/S,Y=Projected.Y/S;
            if(X>375&&X<VW-360&&Y>230&&Y<VH-270)
            {
                Stroke(X-8,Y,X,Y-8,Amber);Stroke(X,Y-8,X+8,Y,Amber);
                Center(FString::Printf(TEXT("CITY  %.0f m"),Distance),X,Y+10,.48f,Amber);
            }
        }
        AVoyagerAnimal* Closest=nullptr;double Best=FMath::Square(16000.0);
        for(TActorIterator<AVoyagerAnimal> It(GetWorld());It;++It)
        {
            const double D=FVector::DistSquared(Position,It->GetActorLocation());
            if(D<Best){Best=D;Closest=*It;}
        }
        if(CityPopulation>0&&Distance<650)
        {
            Text(FString::Printf(TEXT("LOCAL TIME %s  /  %d RESIDENTS NEARBY"),*AVoyagerCitizenManager::ClockText(GetWorld()),CityPopulation),34,253,.46f,Mint);
        }
        else if(Closest)
        {
            Text(FString::Printf(TEXT("FAUNA  /  %s  /  %.0f m"),*Closest->SpeciesName(),FMath::Sqrt(Best)/100.0),34,253,.49f,Mint);
        }
    }

    const float Right = VW - 326;
    Panel(Right, 31, 294, 98);
    Text(TEXT("YOUR EXPEDITION"), Right + 17, 43, .58f, Muted);
    Text(FString::Printf(TEXT("%04d"), Expedition ? Expedition->Minerals : 0), Right + 16, 65, 1.16f, White);
    Text(FString::Printf(TEXT("%03d"), Expedition ? Expedition->Discoveries : 0), Right + 157, 65, 1.16f, Mint);
    Text(TEXT("MINERALS"), Right + 17, 102, .48f, Muted);
    Text(TEXT("DISCOVERIES"), Right + 158, 102, .48f, Muted);
    Text(TEXT("ESC  FLIGHT MANUAL     F5  SAVE"), Right + 3, 142, .53f, Muted);

    if(Expedition&&Expedition->WantedStars>0)
    {
        const float X=VW*.5f-145.f;
        Panel(X,22,290,103);
        for(int32 Star=0;Star<5;++Star)
        {
            const float SX=X+40+Star*52;
            const FLinearColor Color=Star<Expedition->WantedStars?(Expedition->bLawSearching?Amber:Red):Track;
            for(int32 Point=0;Point<10;++Point)
            {
                const float A=-PI*.5f+Point*PI*.2f,B=A+PI*.2f;
                const float R=Point%2?6.f:14.f,R2=Point%2?14.f:6.f;
                Stroke(SX+FMath::Cos(A)*R,49+FMath::Sin(A)*R,SX+FMath::Cos(B)*R2,49+FMath::Sin(B)*R2,Color,2.f);
            }
        }
        Center(Expedition->bLawSearching?TEXT("SEARCHING LAST KNOWN POSITION"):TEXT("WANTED / PATROL PURSUIT"),VW*.5f,74,.53f,Expedition->bLawSearching?Amber:Red,270);
        Center(Expedition->bLawSearching?FString::Printf(TEXT("Stay out of sight  /  %.0f s"),Expedition->WantedSearchSeconds):TEXT("Break line of sight or escape in your ship"),VW*.5f,98,.44f,Muted,270);
        for(TActorIterator<AVoyagerPatrolShip> It(GetWorld());It;++It)
        {
            FVector2D Screen;
            if(It->Hull>0&&It->Suspect==Expedition&&FVector::DistSquared(Position,It->GetActorLocation())<FMath::Square(200000.f)&&PC->ProjectWorldLocationToScreen(It->GetActorLocation(),Screen,true))
            {
                const float PX=Screen.X/S,PY=Screen.Y/S;
                if(PX>20&&PX<VW-20&&PY>140&&PY<VH-180)
                {Stroke(PX-12,PY-10,PX+12,PY-10,Red,2);Stroke(PX-12,PY+10,PX+12,PY+10,Red,2);Center(TEXT("PATROL"),PX,PY+16,.43f,Red);}
            }
        }
    }
    int32 NearbyPirates = 0;
    if (Pawn)
    {
        for (TActorIterator<AVoyagerPirate> It(GetWorld()); It; ++It)
        {
            if (It->Hull <= 0 || FVector::DistSquared(Pawn->GetActorLocation(), It->GetActorLocation()) >= FMath::Square(95000.f)) continue;
            ++NearbyPirates;
            FVector2D EnemyScreen;
            if (PC->ProjectWorldLocationToScreen(It->GetActorLocation(), EnemyScreen, true))
            {
                const float EX = EnemyScreen.X / S, EY = EnemyScreen.Y / S;
                if (EX > 90 && EX < VW - 90 && EY > 210 && EY < VH - 285)
                {
                    Stroke(EX - 21, EY - 21, EX - 9, EY - 21, Red, 1.4f);
                    Stroke(EX - 21, EY - 21, EX - 21, EY - 9, Red, 1.4f);
                    Stroke(EX + 21, EY + 21, EX + 9, EY + 21, Red, 1.4f);
                    Stroke(EX + 21, EY + 21, EX + 21, EY + 9, Red, 1.4f);
                    Center(TEXT("RAIDER"), EX, EY + 29, .48f, Red);
                }
            }
        }
    }
    if (NearbyPirates > 0)
    {
        Panel(VW * .5f - 164, 32, 328, 49);
        Center(FString::Printf(TEXT("HOSTILE SIGNALS  /  %02d"), NearbyPirates), VW * .5f, 47, .68f, Amber);
    }

    const float CX = VW * .5f, CY = VH * .5f;
    FLinearColor Reticle = (Explorer && Explorer->MiningFeedback > 0) || (Ship && Ship->HitFeedback > 0) ? Amber : White;
    Stroke(CX - 14, CY, CX - 6, CY, Reticle, 1.5f);
    Stroke(CX + 6, CY, CX + 14, CY, Reticle, 1.5f);
    Stroke(CX, CY - 14, CX, CY - 6, Reticle, 1.5f);
    Stroke(CX, CY + 6, CX, CY + 14, Reticle, 1.5f);
    Rect(CX - 1, CY - 1, 2, 2, Reticle);

    FString Action, SubAction;
    if (Explorer)
    {
        Panel(32, VH - 155, 290, 109);
        Text(TEXT("EXOSUIT  /  LIFE SUPPORT"), 49, VH - 140, .57f, Muted);
        Text(FString::Printf(TEXT("%03d"), FMath::CeilToInt(Explorer->Health)), 48, VH - 119, 1.6f, Explorer->Health < 30 ? Amber : White);
        Text(TEXT("%"), 146, VH - 97, .67f, Muted);
        Bar(49, VH - 63, 256, Explorer->Health / 100.f, Explorer->Health < 30 ? Amber : Mint);

        Panel(VW - 322, VH - 155, 290, 109);
        Text(Explorer->bWeaponMode?TEXT("PULSE SIDEARM"):TEXT("TERRAIN MULTITOOL"), VW - 305, VH - 140, .57f, Muted);
        Text(Explorer->bWeaponMode?TEXT("LMB  FIRE / V  HOLSTER"):TEXT("LMB EXTRACT / V ARM"), VW - 305, VH - 111, .82f, White);
        Text(TEXT("F  SCAN & DISCOVER"), VW - 305, VH - 76, .63f, Mint);

        Action = TEXT("EXPLORE  /  SCAN  /  COLLECT");
        SubAction = TEXT("WASD move   -   SHIFT sprint   -   SPACE jump   -   C camera");
        float NearestShip = TNumericLimits<float>::Max();
        AVoyagerShip* ParkedShip = nullptr;
        for (TActorIterator<AVoyagerShip> It(GetWorld()); It; ++It)
        {
            const float D = FVector::DistSquared(Explorer->GetActorLocation(), It->GetActorLocation());
            if (It->bLanded && It->GetOwner() == PC && D < NearestShip)
            {
                NearestShip = D;
                ParkedShip = *It;
            }
        }
        if (ParkedShip && NearestShip < 1100.f * 1100.f)
        {
            Action = TEXT("E  BOARD YOUR STARSHIP");
            SubAction = TEXT("Take the controls. A whole system is waiting.");
        }
        else if (ParkedShip)
        {
            FVector2D ShipScreen;
            const FVector ShipUp = Voyager::SurfaceNormal(State->SystemSeed, NearestPlanet, ParkedShip->GetActorLocation());
            bool bAhead = PC->ProjectWorldLocationToScreen(ParkedShip->GetActorLocation() + ShipUp * 180.0, ShipScreen, true);
            float SX = ShipScreen.X / S, SY = ShipScreen.Y / S;
            if (!bAhead)
            {
                FVector ViewLocation; FRotator ViewRotation;
                PC->GetPlayerViewPoint(ViewLocation, ViewRotation);
                const float Side = FVector::DotProduct(ParkedShip->GetActorLocation() - ViewLocation, FRotationMatrix(ViewRotation).GetUnitAxis(EAxis::Y));
                SX = Side >= 0 ? VW - 110 : 110;
                SY = CY;
            }
            SX = FMath::Clamp(SX, 110.f, VW - 110.f);
            SY = FMath::Clamp(SY, 230.f, VH - 270.f);
            Stroke(SX - 8, SY + 6, SX, SY - 8, Mint, 1.5f);
            Stroke(SX, SY - 8, SX + 8, SY + 6, Mint, 1.5f);
            Stroke(SX + 8, SY + 6, SX - 8, SY + 6, Mint, 1.5f);
            Center(FString::Printf(TEXT("STARSHIP  /  %.0f m"), FMath::Sqrt(NearestShip) / 100.f), SX, SY + 19, .50f, Mint);
        }
        if (Explorer->Camera)
        {
            FHitResult Hit;
            FCollisionQueryParams Params;
            Params.AddIgnoredActor(Explorer);
            const FVector Start = Explorer->Camera->GetComponentLocation();
            if (GetWorld()->LineTraceSingleByChannel(Hit, Start, Start + Explorer->Camera->GetForwardVector() * 1800.f, ECC_Visibility, Params))
            {
                if (auto* Resource = Cast<AVoyagerResource>(Hit.GetActor()))
                {
                    if (!Resource->bHarvested)
                    {
                        Center(Resource->ResourceName().ToUpper(), CX, CY + 42, .74f, Mint);
                        Center(TEXT("HOLD LMB  /  EXTRACT MINERALS"), CX, CY + 69, .53f, White);
                    }
                }
            }
        }
        if (Explorer->ScanPulse > 0)
        {
            const float Time = GetWorld()->GetTimeSeconds();
            const float Radius = 100.f + FMath::Fmod(Time * 180.f, 250.f);
            const FLinearColor ScanColor(.37f, 1.f, .79f, .3f);
            for (int32 I = 0; I < 64; ++I)
            {
                const float A = 2.f * PI * I / 64.f, B = 2.f * PI * (I + 1) / 64.f;
                Stroke(CX + FMath::Cos(A) * Radius, CY + FMath::Sin(A) * Radius,
                       CX + FMath::Cos(B) * Radius, CY + FMath::Sin(B) * Radius, ScanColor, 1.3f);
            }
            Center(TEXT("PLANETARY SURVEY ACTIVE"), CX, CY - 125, .67f, Mint);
        }
    }

    if (Ship)
    {
        const float TelemetryX = VW - 322;
        const double SpeedMeters = DisplayedSpeedCm / 100.0;
        const bool bKilometersPerSecond = SpeedMeters >= 1000.0;
        Panel(TelemetryX, VH - 224, 290, 178);
        Text(Ship->bCruising ? TEXT("AUTOPILOT  /  CRUISE") : TEXT("STARSHIP TELEMETRY"), TelemetryX + 17, VH - 208, .57f, Muted);
        Text(bKilometersPerSecond ? FString::Printf(TEXT("%.1f"), SpeedMeters / 1000.0) : FString::Printf(TEXT("%.0f"), SpeedMeters), TelemetryX + 16, VH - 185, 1.5f, White);
        Text(bKilometersPerSecond ? TEXT("km/s") : TEXT("m/s"), TelemetryX + 183, VH - 163, .61f, Muted);
        Text(FString::Printf(TEXT("SHIELD  %03d"), FMath::CeilToInt(Ship->Shield)), TelemetryX + 17, VH - 125, .59f, Mint);
        Bar(TelemetryX + 17, VH - 101, 254, Ship->Shield / 100.f, Mint);
        Text(FString::Printf(TEXT("HULL     %03d"), FMath::CeilToInt(Ship->Hull)), TelemetryX + 17, VH - 88, .59f, Ship->Hull < 35 ? Red : White);
        Bar(TelemetryX + 17, VH - 63, 254, Ship->Hull / 100.f, Ship->Hull < 35 ? Red : White);

        // Flight context always belongs to the closest physical body. Navigation
        // selection changes the destination only, never these altitude readings.
        Panel(Right, 185, 294, 179);
        Text(TEXT("ALTITUDE ABOVE LOCAL TERRAIN"), Right + 17, 199, .53f, Muted);
        Text(AltitudeCm < 100000.0 ? FString::Printf(TEXT("%.0f m"), AltitudeCm / 100.0) : FString::Printf(TEXT("%.2f km"), AltitudeKm), Right + 16, 226, 1.22f, White);
        Text(bReentry ? TEXT("REENTRY  /  DESCENDING") : Zone, Right + 17, 269, .65f, bReentry ? Amber : Mint);
        Text(FString::Printf(TEXT("AIR DENSITY  %.1f%%"), AirDensity * 100.f), Right + 17, 299, .53f, Muted);
        Bar(Right + 17, 326, 260, AirDensity, bReentry ? Amber : Mint);
        Text(TEXT("60 km  /  OUTER ATMOSPHERE"), Right + 17, 341, .47f, Muted);

        const int32 Target = FMath::Clamp(Ship->TargetPlanet, 0, Voyager::PlanetCount - 1);
        Panel(32, VH - 264, 337, 218);
        Text(TEXT("SYSTEM NAVIGATION"), 49, VH - 249, .57f, Muted);
        Text(TEXT("TAB  SELECT DESTINATION"), 49, VH - 225, .52f, Mint);
        for (int32 I = 0; I < Voyager::PlanetCount; ++I)
        {
            const float Y = VH - 196 + I * 29.f;
            if (I == Target) { Rect(42, Y - 3, 317, 28, FLinearColor(.06f, .25f, .23f, .8f)); Rect(42, Y - 3, 2, 28, Mint); }
            Text(FString::Printf(TEXT("%02d"), I + 1), 52, Y, .57f, I == Target ? Mint : Muted);
            Text(Voyager::PlanetName(State->SystemSeed, I), 88, Y, .64f, I == Target ? White : Muted);
            if (I == NearestPlanet) Text(TEXT("LOCAL"), 290, Y + 2, .43f, Mint);
        }

        const FVector Destination = Voyager::PlanetCenter(State->SystemSeed, Target);
        const double SurfaceDistance = FMath::Max(0.0, FVector::Dist(Position, Destination) - Voyager::PlanetRadius(State->SystemSeed, Target));
        if (Ship->bLanded)
        {
            Action = TEXT("SPACE  LAUNCH    /    E  DISEMBARK");
            SubAction = TEXT("Your ship rises away from the planet beneath you.");
        }
        else if (bNearGround)
        {
            Action = TEXT("E  LAND & DISEMBARK    /    SPACE  RISE");
            SubAction = TEXT("Touch down anywhere. Your ship stays where you park.");
        }
        else if (Ship->bCruising)
        {
            Action = TEXT("CRUISE TO ") + Voyager::PlanetName(State->SystemSeed, Target).ToUpper();
            SubAction = TEXT("Continuous flight to 90 km altitude  /  J cancel cruise");
        }
        else if (!bSpace)
        {
            Action = bReentry ? TEXT("REENTRY    /    CTRL  DESCEND    /    S  BRAKE") : TEXT("SPACE  CLIMB    /    CTRL  DESCEND");
            SubAction = TEXT("SHIFT boosts ascent  /  Cruise becomes available above 60 km");
        }
        else
        {
            Action = TEXT("J  CRUISE TO SELECTED PLANET");
            SubAction = TEXT("TAB select world   -   CTRL descend   -   H jump to next star");
        }

        // A destination marker is useful in transit. Hide a selected local
        // planet's center below your feet while flying through its atmosphere.
        if (Target != NearestPlanet || bSpace)
        {
            FVector2D Projected;
            const bool bVisible = PC->ProjectWorldLocationToScreen(Destination, Projected, true);
            float PX = Projected.X / S, PY = Projected.Y / S;
            const float XMin = 420.f, XMax = VW - 380.f, YMin = 235.f, YMax = VH - 300.f;
            const bool bOnScreen = bVisible && PX > XMin && PX < XMax && PY > YMin && PY < YMax;
            if (!bOnScreen)
            {
                FVector ViewLocation; FRotator ViewRotation;
                PC->GetPlayerViewPoint(ViewLocation, ViewRotation);
                const FVector Direction = (Destination - ViewLocation).GetSafeNormal();
                const FRotationMatrix Rotation(ViewRotation);
                float DX = FVector::DotProduct(Direction, Rotation.GetUnitAxis(EAxis::Y));
                float DY = -FVector::DotProduct(Direction, Rotation.GetUnitAxis(EAxis::Z));
                if (FMath::Abs(DX) + FMath::Abs(DY) < .05f) DX = 1.f;
                const float EdgeX = FMath::Max(80.f, (XMax - XMin) * .5f);
                const float EdgeY = FMath::Max(60.f, (YMax - YMin) * .5f);
                const float Factor = 1.f / FMath::Max(FMath::Abs(DX) / EdgeX, FMath::Abs(DY) / EdgeY);
                PX = (XMin + XMax) * .5f + DX * Factor;
                PY = (YMin + YMax) * .5f + DY * Factor;
            }
            const FLinearColor Marker = Ship->bCruising ? Mint : White;
            Stroke(PX, PY - 12, PX + 12, PY, Marker, 1.5f);
            Stroke(PX + 12, PY, PX, PY + 12, Marker, 1.5f);
            Stroke(PX, PY + 12, PX - 12, PY, Marker, 1.5f);
            Stroke(PX - 12, PY, PX, PY - 12, Marker, 1.5f);
            Center(Voyager::PlanetName(State->SystemSeed, Target).ToUpper(), PX, PY + 24, .71f, Marker);
            Center(FString::Printf(TEXT("%.1f km TO SURFACE"), SurfaceDistance / 100000.0), PX, PY + 51, .52f, Muted);
            Center(FString::Printf(TEXT("DIAMETER %.0f km"), Voyager::PlanetRadius(State->SystemSeed, Target) * 2.0 / 100000.0), PX, PY + 72, .46f, Muted);
        }
    }

    if(Explorer)
    {
        if(auto* Citizen=Explorer->FocusedCitizen())
        {
            Action=TEXT("E  TALK TO ")+Citizen->DisplayName().ToUpper();
            SubAction=Citizen->RoleName()+TEXT("  /  ")+Citizen->ActivityName();
        }
        else if(InsideBuilding!=INDEX_NONE)
        {
            Action=InteriorName.ToUpper();
            SubAction=TEXT("Explore the interior  /  E talk to residents");
        }
        else if(ClosestBuilding!=INDEX_NONE&&FVector::DistSquared(Position,ClosestDoor)<FMath::Square(8500.0))
        {
            FVector2D DoorScreen;
            if(PC->ProjectWorldLocationToScreen(ClosestDoor+Explorer->GetActorUpVector()*300,DoorScreen,true))
            {
                const float X=DoorScreen.X/S,Y=DoorScreen.Y/S;
                if(X>450&&X<VW-340&&Y>290&&Y<VH-250)
                    Center(TEXT("OPEN ENTRANCE"),X,Y,.46f,Amber);
            }
        }
    }
    if (!Action.IsEmpty())
    {
        const float ActionWidth = FMath::Min(790.f, VW - 760.f);
        Panel(CX - ActionWidth * .5f, VH - 149, ActionWidth, 78);
        Center(Action, CX, VH - 132, .77f, Mint, ActionWidth - 28);
        Center(SubAction, CX, VH - 99, .48f, White, ActionWidth - 28);
    }
    if(Explorer&&PC->ConversationTime>0&&PC->ConversationTarget.IsValid())
    {
        const float BoxWidth=FMath::Min(860.f,VW-100.f),Left=CX-BoxWidth*.5f,Top=VH-378;
        Panel(Left,Top,BoxWidth,204);
        Text(PC->ConversationTarget->DisplayName()+TEXT("  /  ")+PC->ConversationTarget->RoleName(),Left+22,Top+16,.70f,Amber);
        TArray<FString> Words;PC->ConversationSpeech.ParseIntoArrayWS(Words);
        FString CurrentLine;float Y=Top+51;
        for(const FString& Word:Words)
        {
            const FString Candidate=CurrentLine.IsEmpty()?Word:CurrentLine+TEXT(" ")+Word;
            float TW=0,TH=0;GetTextSize(Candidate,TW,TH,GEngine->GetMediumFont(),.62f*S*1.8f);
            if(TW>(BoxWidth-44)*S&&!CurrentLine.IsEmpty())
            {Text(CurrentLine,Left+22,Y,.62f,White);Y+=25;CurrentLine=Word;}
            else CurrentLine=Candidate;
        }
        if(!CurrentLine.IsEmpty())Text(CurrentLine,Left+22,Y,.62f,White);
        Text(TEXT("1  GREET     2  DAILY LIFE     3  LOCAL ADVICE"),Left+22,Top+148,.52f,Mint);
        Text(TEXT("BACKSPACE  END CONVERSATION     /     WALK AWAY TO LEAVE"),Left+22,Top+178,.43f,Muted);
    }
    else if (PC->NoticeTime > 0 && !PC->Notice.IsEmpty())
    {
        Panel(CX - 360, VH - 226, 720, 52);
        Center(PC->Notice, CX, VH - 210, .69f, White, 684);
    }
    const FString Footer = Ship ? TEXT("WASD  THRUST     MOUSE  STEER     SHIFT  BOOST     SPACE / CTRL  VERTICAL     LMB  FIRE") : TEXT("WASD  MOVE     MOUSE  LOOK     SHIFT  SPRINT     SPACE  JUMP     E  INTERACT     F  SCAN");
    Center(Footer, CX, VH - 27, .48f, Muted);

    if (Ship && Ship->DamageFeedback > 0)
    {
        const FLinearColor DamageTint(1.f, .08f, .03f, FMath::Clamp(Ship->DamageFeedback, 0.f, 1.f) * .5f);
        Rect(0, 0, VW, 5, DamageTint);
        Rect(0, VH - 5, VW, 5, DamageTint);
        Rect(0, 0, 5, VH, DamageTint);
        Rect(VW - 5, 0, 5, VH, DamageTint);
    }

    if (State->bTransitioning)
    {
        Rect(0, 0, VW, VH, FLinearColor(.005f, .012f, .026f, .95f));
        for (int32 I = 0; I < 22; ++I)
        {
            const float Y = FMath::Fmod(I * 91.f + GetWorld()->GetTimeSeconds() * (25.f + I), VH);
            const float X = FMath::Fmod(I * 263.f, VW);
            Rect(X, Y, 1.f, 18.f + I * 2.f, FLinearColor(.28f, .67f, .71f, .15f));
        }
        Center(TEXT("R I F T B O U N D   /   V O Y A G E R"), CX, VH * .31f, .67f, Muted);
        Center(State->TransitionLabel.ToUpper(), CX, VH * .44f, 1.6f, White);
        Center(TEXT("A NEW HORIZON IS FORMING"), CX, VH * .44f + 65, .65f, Mint);
        Bar(CX - 225, VH * .44f + 112, 450, 1.f - State->TransitionTime / 3.f, Mint);
        Center(Voyager::SystemName(State->SystemSeed), CX, VH * .44f + 139, .62f, Muted);
    }
}
