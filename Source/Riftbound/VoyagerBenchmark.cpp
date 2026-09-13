// Opt-in reproducible scene sampler. Rendering measurements never enter simulation state.
#include "VoyagerCharacter.h"
#include "VoyagerGameMode.h"
#include "VoyagerCityLife.h"
#include "VoyagerSettlement.h"
#include "VoyagerData.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/HUD.h"
#include "HAL/IConsoleManager.h"
#include "GPUProfiler.h"
#include "RenderTimer.h"
#include "RHI.h"
#include "RHIStats.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "UnrealClient.h"

namespace VoyagerBenchmark {
const TCHAR* Names[]={TEXT("Wilderness"),TEXT("CityExterior"),TEXT("Interior"),TEXT("Orbit")};
struct FRun {TWeakObjectPtr<UWorld> World;int32 Scene=-1;double Start=0,Previous=0;bool Done=false;
    FRotator Camera=FRotator::ZeroRotator;
    TArray<double> Frames,GPU;double GameMs=0,RenderMs=0;FRHIGPUFrameTimeHistory::FState GPUState;
    TArray<TSharedPtr<FJsonValue>> Results;};
TUniquePtr<FRun> Run;
void Place(UWorld* W,AVoyagerController* PC,AVoyagerCharacter* Pawn,int32 Scene) {
    auto State=W->GetGameState<AVoyagerState>();const int32 S=State->SystemSeed,P=State->PlanetIndex;
    FVector Up=FVector::UpVector,Forward=FVector::ForwardVector,At;
    if(Scene==0)At=Voyager::SurfacePoint(S,P,FVector::UpVector)+FVector(-22000,12000,130);
    else if(Scene==1)At=AVoyagerSettlement::StreetPoint(S,P,0,4,3,120);
    else if(Scene==2){FVoyagerBuildingInfo Info;AVoyagerSettlement::GetBuildingInfo(S,P,0,0,Info);At=Info.InteriorPoint+Voyager::SurfaceNormal(S,P,Info.InteriorPoint)*110;}
    else {At=Voyager::SurfacePoint(S,P,FVector::UpVector,9000000);Forward=FVector(1,0,-.65).GetSafeNormal();}
    Up=Voyager::SurfaceNormal(S,P,At);const auto Facing=Voyager::TangentRotation(Up,Forward);
    Pawn->GetCharacterMovement()->StopMovementImmediately();Pawn->GetCharacterMovement()->SetGravityDirection(-Up);
    Pawn->GetCharacterMovement()->SetMovementMode(Scene==3?MOVE_Flying:MOVE_Falling);
    Run->Camera=Scene==3?Forward.Rotation():Facing;
    PC->SetIgnoreLookInput(true);PC->SetIgnoreMoveInput(true);
    if(PC->GetHUD())PC->GetHUD()->bShowHUD=false;
    Pawn->SetActorLocationAndRotation(At,Facing,false,nullptr,ETeleportType::TeleportPhysics);PC->SetControlRotation(Run->Camera);
}
void FinishScene(UWorld* W,AVoyagerController* PC) {
    auto Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("scene"),Names[Run->Scene]);
    FVector Eye;FRotator View;PC->GetPlayerViewPoint(Eye,View);
    Row->SetNumberField(TEXT("view_alignment"),FVector::DotProduct(View.Vector(),Run->Camera.Vector()));
    Row->SetStringField(TEXT("camera_position"),Eye.ToString());
    auto Summarize=[&](const TCHAR* Prefix,TArray<double>& Values) {
        if(Values.IsEmpty()){Row->SetField(FString(Prefix)+TEXT("_mean_ms"),MakeShared<FJsonValueNull>());return;}
        double Sum=0;for(double V:Values)Sum+=V;Values.Sort();
        Row->SetNumberField(FString(Prefix)+TEXT("_mean_ms"),Sum/Values.Num());
        Row->SetNumberField(FString(Prefix)+TEXT("_p95_ms"),Values[FMath::Min(Values.Num()-1,FMath::FloorToInt(Values.Num()*.95))]);
        Row->SetNumberField(FString(Prefix)+TEXT("_max_ms"),Values.Last());
    };
    Summarize(TEXT("wall_frame"),Run->Frames);Summarize(TEXT("gpu_busy"),Run->GPU);
    Row->SetNumberField(TEXT("frames"),Run->Frames.Num());Row->SetNumberField(TEXT("gpu_samples"),Run->GPU.Num());
    Row->SetNumberField(TEXT("game_thread_mean_ms"),Run->GameMs/FMath::Max(1,Run->Frames.Num()));
    Row->SetNumberField(TEXT("render_thread_mean_ms"),Run->RenderMs/FMath::Max(1,Run->Frames.Num()));
    Row->SetNumberField(TEXT("process_resident_mib"),FPlatformMemory::GetStats().UsedPhysical/1048576.0);
    int32 Actors=0,Components=0;for(TActorIterator<AActor> It(W);It;++It){++Actors;Components+=It->GetComponents().Num();}
    Row->SetNumberField(TEXT("actors"),Actors);Row->SetNumberField(TEXT("components"),Components);
    Row->SetNumberField(TEXT("last_rhi_draw_calls"),GNumDrawCallsRHI[0]);
    FTextureMemoryStats Memory;RHIGetTextureMemoryStats(Memory);
    Row->SetNumberField(TEXT("streaming_texture_mib"),Memory.StreamingMemorySize/1048576.0);
    Row->SetNumberField(TEXT("nonstreaming_texture_mib"),Memory.NonStreamingMemorySize/1048576.0);
    if(auto Life=AVoyagerCityLife::Find(W))Row->SetNumberField(TEXT("last_city_worker_ms"),Life->BuildView(PC).TickMilliseconds);
    Run->Results.Add(MakeShared<FJsonValueObject>(Row));
    FString Folder;FParse::Value(FCommandLine::Get(),TEXT("VoyagerBenchmarkOutput="),Folder);
    FScreenshotRequest::RequestScreenshot(Folder/(FString(Names[Run->Scene])+TEXT(".png")),false,false);
    UE_LOG(LogTemp,Display,TEXT("VOYAGER BENCHMARK SCENE %s frames=%d gpu_samples=%d"),Names[Run->Scene],Run->Frames.Num(),Run->GPU.Num());
}
void Tick(UWorld* W,ELevelTick,float) {
    if(!FParse::Param(FCommandLine::Get(),TEXT("VoyagerBenchmark"))||!W||!W->IsGameWorld()||W->GetNetMode()!=NM_Standalone)return;
    auto PC=Cast<AVoyagerController>(W->GetFirstPlayerController());auto Pawn=PC?Cast<AVoyagerCharacter>(PC->GetPawn()):nullptr;
    if(!Pawn||W->GetTimeSeconds()<9)return;
    if(!Run){Run=MakeUnique<FRun>();Run->World=W;}
    if(Run->World.Get()!=W||Run->Done)return;
    const double Now=FPlatformTime::Seconds();
    if(Run->Scene<0){Run->Scene=0;Run->Start=Run->Previous=Now;Place(W,PC,Pawn,0);}
    const double Elapsed=Now-Run->Start;uint64 GPUCycles;
    while(Run->GPUState.PopFrameCycles(GPUCycles)!=FRHIGPUFrameTimeHistory::EResult::Empty)
        if(Elapsed>=8&&Elapsed<18&&GPUCycles>0)Run->GPU.Add(FPlatformTime::ToMilliseconds64(GPUCycles));
    if(Elapsed>=8&&Elapsed<18) {
        Run->Frames.Add((Now-Run->Previous)*1000);Run->GameMs+=FPlatformTime::ToMilliseconds(GGameThreadTime);Run->RenderMs+=FPlatformTime::ToMilliseconds(GRenderThreadTime);
    }
    Run->Previous=Now;
    if(Elapsed>=18&&Run->Frames.Num()) {FinishScene(W,PC);Run->Frames.Reset();Run->GPU.Reset();}
    if(Elapsed<19)return;
    if(++Run->Scene<4){Run->Start=Now;Run->GameMs=Run->RenderMs=0;Place(W,PC,Pawn,Run->Scene);return;}
    auto Report=MakeShared<FJsonObject>();auto State=W->GetGameState<AVoyagerState>();
    Report->SetNumberField(TEXT("system"),State->SystemSeed);Report->SetNumberField(TEXT("planet"),State->PlanetIndex);
    Report->SetStringField(TEXT("sampling"),TEXT("8s settle + 10s sample per scene; wall frame includes cap/wait; thread times exclude idle; GPU busy from RHI history; counts/memory are end-of-scene samples"));
    Report->SetArrayField(TEXT("scenes"),Run->Results);
    Report->SetNumberField(TEXT("quality_tier"),PC->GraphicsLevel);
    auto Settings=MakeShared<FJsonObject>();
    for(const TCHAR* Name:{TEXT("r.Streaming.PoolSize"),TEXT("r.ViewDistanceScale"),TEXT("r.SkyAtmosphere.SampleCountMax"),TEXT("r.VolumetricCloud.ViewRaySampleMaxCount"),TEXT("r.ScreenPercentage")})
        if(auto Variable=IConsoleManager::Get().FindConsoleVariable(Name))Settings->SetNumberField(Name,Variable->GetFloat());
    Report->SetObjectField(TEXT("render_settings"),Settings);
    FVector2D Size=FVector2D::ZeroVector;if(GEngine&&GEngine->GameViewport)GEngine->GameViewport->GetViewportSize(Size);
    Report->SetNumberField(TEXT("width"),Size.X);Report->SetNumberField(TEXT("height"),Size.Y);
    FString Folder,Text;FParse::Value(FCommandLine::Get(),TEXT("VoyagerBenchmarkOutput="),Folder);FJsonSerializer::Serialize(Report,TJsonWriterFactory<>::Create(&Text));
    const bool Saved=FFileHelper::SaveStringToFile(Text,*(Folder/TEXT("Measurements.json")));
    Run->Done=true;UE_LOG(LogTemp,Display,TEXT("VOYAGER BENCHMARK COMPLETE saved=%d"),Saved);FPlatformMisc::RequestExit(false);
}
struct FRegistration {FDelegateHandle Handle;FRegistration(){Handle=FWorldDelegates::OnWorldPostActorTick.AddStatic(&Tick);}~FRegistration(){FWorldDelegates::OnWorldPostActorTick.Remove(Handle);}} Registration;
}
