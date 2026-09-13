#include "VoyagerLanding.h"
#include "VoyagerShip.h"
#include "VoyagerGameMode.h"
#include "VoyagerData.h"
#include "VoyagerWorld.h"
#include "livingcity/world/Landing.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace VoyagerLanding {
namespace {
const lc::LandingLimits& Limits() {
    static const lc::LandingLimits Value=[] {
        lc::LandingLimits L;FString Text;TSharedPtr<FJsonObject> Json;
        if(FFileHelper::LoadFileToString(Text,*(FPaths::ProjectContentDir()/TEXT("CityData/flight_safety.json")))&&
            FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Json)&&Json.IsValid()) {
            auto Read=[&](const TCHAR* Key,int32& V,int32 Low,int32 High){double N;if(Json->TryGetNumberField(Key,N)&&FMath::IsFinite(N))V=FMath::Clamp(int32(FMath::Clamp(N,double(Low),double(High))),Low,High);};
            Read(TEXT("maximum_landing_altitude_cm"),L.maxAltitudeCm,200,2000);
            Read(TEXT("maximum_landing_speed_cm_per_second"),L.maxSpeedCmPerSecond,50,1000);
            Read(TEXT("maximum_support_relief_cm"),L.maxReliefCm,0,150);
            Read(TEXT("minimum_surface_up_dot_milli"),L.minimumUpDotMilli,940,1000);
        } else UE_LOG(LogTemp,Warning,TEXT("VOYAGER LANDING using default safety limits: flight_safety.json unavailable"));
        return L;
    }();return Value;
}
FVector Ground(int32 System,int32 Planet,const FVector& At) {
    return Voyager::SurfacePoint(System,Planet,Voyager::SurfaceNormal(System,Planet,At));
}
int32 Quantize(double Value) {return int32(FMath::Clamp(Value,-2147483647.0,2147483647.0));}
}
bool FindLanding(const AVoyagerShip* Ship,FVector& Location,FRotator& Rotation,FString& Reason) {
    if(!Ship||!Ship->HasAuthority())return false;
    auto W=Ship->GetWorld();auto State=W?W->GetGameState<AVoyagerState>():nullptr;if(!State)return false;
    const int32 System=State->SystemSeed,Planet=Ship->NearestPlanetIndex();
    const FVector At=Ship->GetActorLocation(),Up=Voyager::SurfaceNormal(System,Planet,At);
    lc::LandingObservation Observation;
    Observation.finite=!At.ContainsNaN()&&!Ship->GetVelocity().ContainsNaN();
    if(!Observation.finite){Reason=TEXT("Landing unavailable: invalid flight state.");return false;}
    Observation.altitudeCm=Quantize(Ship->SurfaceAltitude());Observation.speedCmPerSecond=Quantize(Ship->GetVelocity().Size());
    Rotation=Voyager::TangentRotation(Up,Ship->GetActorForwardVector());
    const FVector Base=Ground(System,Planet,At);Location=Base+Up*160;
    // Cover the full 7.4 m x 7.9 m airframe, including its wings, not just its center.
    int32 Index=0;
    for(int32 X=-1;X<=1;++X)for(int32 Y=-1;Y<=1;++Y) {
        const FVector Sample=Base+Rotation.RotateVector(FVector(X*375,Y*400,0));
        const FVector G=Ground(System,Planet,Sample);
        Observation.supportHeightsCm[Index++]=Quantize(FVector::DotProduct(G-Base,Up));
        const FVector GX=Ground(System,Planet,Sample+Rotation.Vector()*50);
        const FVector GY=Ground(System,Planet,Sample+Rotation.RotateVector(FVector(0,50,0)));
        const FVector Normal=FVector::CrossProduct(GX-G,GY-G).GetSafeNormal();
        Observation.minimumUpDotMilli=FMath::Min(Observation.minimumUpDotMilli,Quantize(FVector::DotProduct(Normal,Up)*1000));
    }
    FCollisionQueryParams Query(SCENE_QUERY_STAT(VoyagerLandingFootprint),false,Ship);
    // The analytical support samples above include terrain even while its collision LOD streams.
    for(TActorIterator<AVoyagerWorld> It(W);It;++It)Query.AddIgnoredActor(*It);
    const FCollisionShape Hull=FCollisionShape::MakeBox(FVector(375,400,160));
    const FVector Offset=Up*85;
    FHitResult Hit;
    Observation.footprintClear=!W->OverlapBlockingTestByChannel(Location+Offset,Rotation.Quaternion(),ECC_Pawn,Hull,Query)&&
        !W->SweepSingleByChannel(Hit,At+Offset,Location+Offset,Rotation.Quaternion(),ECC_Pawn,Hull,Query);
    switch(lc::EvaluateLanding(Observation,Limits())) {
    case lc::LandingDecision::Safe:return true;
    case lc::LandingDecision::TooHigh:Reason=TEXT("Descend below 10 m to land.");break;
    case lc::LandingDecision::TooFast:Reason=TEXT("Brake and release thrust: landing requires less than 5 m/s.");break;
    case lc::LandingDecision::Steep:case lc::LandingDecision::Uneven:Reason=TEXT("Ground is too steep or uneven. Find a level clearing.");break;
    case lc::LandingDecision::Obstructed:Reason=TEXT("Landing area blocked. Move clear of structures, rocks and trees.");break;
    default:Reason=TEXT("Landing unavailable: unsafe ground observation.");break;
    }
    return false;
}
bool FindExit(const AVoyagerShip* Ship,const FVector& Location,const FRotator& Rotation,FVector& Exit) {
    if(!Ship||!Ship->HasAuthority())return false;
    auto W=Ship->GetWorld();auto State=W?W->GetGameState<AVoyagerState>():nullptr;if(!State)return false;
    const int32 System=State->SystemSeed,Planet=Voyager::NearestPlanet(System,Location);
    const FVector Up=Voyager::SurfaceNormal(System,Planet,Location);
    const FVector Offsets[]={FVector(0,-520,0),FVector(0,520,0),FVector(540,0,0),FVector(-540,0,0),
        FVector(450,-520,0),FVector(450,520,0),FVector(-450,-520,0),FVector(-450,520,0)};
    FCollisionQueryParams Query(SCENE_QUERY_STAT(VoyagerExitClearance),false,Ship);
    const FCollisionShape Capsule=FCollisionShape::MakeCapsule(36,90);
    for(const FVector& Offset:Offsets) {
        const FVector Candidate=Ground(System,Planet,Location+Rotation.RotateVector(Offset));
        if(FMath::Abs(FVector::DotProduct(Candidate-(Location-Up*160),Up))>120)continue;
        FVector Floor=Candidate;FHitResult Hit;
        // A valid analytical height alone does not mean collision has finished streaming.
        if(!W->LineTraceSingleByChannel(Hit,Candidate+Up*220,Candidate-Up*120,ECC_Visibility,Query))continue;
        if(FVector::DotProduct(Hit.ImpactNormal,Up)<.9||FMath::Abs(FVector::DotProduct(Hit.ImpactPoint-Candidate,Up))>100)continue;
        Floor=Hit.ImpactPoint;
        const FVector Center=Floor+Up*110;
        if(W->OverlapBlockingTestByChannel(Center,Rotation.Quaternion(),ECC_Pawn,Capsule,Query))continue;
        if(W->SweepSingleByChannel(Hit,Location+Up*50,Center,Rotation.Quaternion(),ECC_Pawn,Capsule,Query))continue;
        Exit=Center;return true;
    }
    return false;
}
}
