#include "VoyagerVegetation.h"
#include "VoyagerData.h"
#include "VoyagerGameMode.h"
#include "VoyagerSettlement.h"
#include "Components/CapsuleComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "PhysicsEngine/BodyInstance.h"

namespace
{
    enum ENatureMesh : int32 { Broadleaf,Conifer,DryShrub,Grass,Fern,Boulder,MeshCount };
    constexpr int32 CanopyLevel=13, GroundLevel=15;
    constexpr int32 MaxCanopyCells=96, MaxGroundCells=64;
    constexpr int32 MaxGroundInstancesPerCell=768;
    constexpr int32 MaxTreesPerView=64, MaxPlayerViews=4;
    constexpr double CanopyRange=65000., GroundRange=11000., TrunkRange=6500.;
    constexpr double CanopyAltitude=85000., GroundAltitude=11000.;
    // Until standing oceans are installed this reserves wet low basins on the
    // two humid climate families. No ocean surface or planet radius is changed.
    constexpr double WetBasinLevel=-25000.;
    constexpr uint64 CoordinateMask=(1ull<<18)-1;

    uint64 CellKey(int32 Planet,int32 Face,int32 Level,int32 X,int32 Y)
    { return uint64(Y)|(uint64(X)<<18)|(uint64(Level)<<36)|(uint64(Face)<<41)|(uint64(Planet)<<44); }
    int32 PlanetOf(uint64 K) { return int32((K>>44)&7); }
    int32 FaceOf(uint64 K) { return int32((K>>41)&7); }
    int32 LevelOf(uint64 K) { return int32((K>>36)&31); }
    int32 XOf(uint64 K) { return int32((K>>18)&CoordinateMask); }
    int32 YOf(uint64 K) { return int32(K&CoordinateMask); }
    FVector CubeDirection(int32 Face,double U,double V)
    {
        switch(Face)
        {
        case 0:return FVector(1,U,V).GetSafeNormal();
        case 1:return FVector(-1,-U,V).GetSafeNormal();
        case 2:return FVector(-U,1,V).GetSafeNormal();
        case 3:return FVector(U,-1,V).GetSafeNormal();
        case 4:return FVector(U,V,1).GetSafeNormal();
        default:return FVector(U,-V,-1).GetSafeNormal();
        }
    }
    int32 DominantFace(FVector D)
    {
        const FVector A=D.GetAbs();
        if(A.X>=A.Y&&A.X>=A.Z)return D.X>=0?0:1;
        if(A.Y>=A.Z)return D.Y>=0?2:3;
        return D.Z>=0?4:5;
    }
    void FaceUV(FVector D,int32 F,double& U,double& V)
    {
        double Axis=1;
        switch(F)
        {
        case 0:Axis=D.X;U=D.Y;V=D.Z;break;
        case 1:Axis=-D.X;U=-D.Y;V=D.Z;break;
        case 2:Axis=D.Y;U=-D.X;V=D.Z;break;
        case 3:Axis=-D.Y;U=D.X;V=D.Z;break;
        case 4:Axis=D.Z;U=D.X;V=D.Y;break;
        default:Axis=-D.Z;U=D.X;V=-D.Y;break;
        }
        U/=FMath::Max(Axis,1.e-12);V/=FMath::Max(Axis,1.e-12);
    }
    FVector CellDirection(uint64 K)
    {
        const double Span=2./double(1<<LevelOf(K));
        return CubeDirection(FaceOf(K),-1.+(XOf(K)+.5)*Span,-1.+(YOf(K)+.5)*Span);
    }
    bool ViewMoved(const TArray<FVector>& Current,const TArray<FVector>& Previous,double Threshold)
    {
        if(Current.Num()!=Previous.Num())return true;
        for(int32 I=0;I<Current.Num();++I)
            if(FVector::DistSquared(Current[I],Previous[I])>Threshold*Threshold)return true;
        return false;
    }

    /** Build candidates on the actual cube faces, including across their seams. */
    void NearbyCells(int32 System,FVector View,int32 Level,double Range,TMap<uint64,double>& Out)
    {
        const int32 Planet=Voyager::NearestPlanet(System,View);
        const double Radius=Voyager::PlanetRadius(System,Planet);
        const FVector Up=Voyager::SurfaceNormal(System,Planet,View);
        const int32 Face=DominantFace(Up),Count=1<<Level;
        double U=0,V=0;FaceUV(Up,Face,U,V);
        const double Span=2./Count;
        const int32 CX=FMath::Clamp(FMath::FloorToInt((U+1.)*.5*Count),0,Count-1);
        const int32 CY=FMath::Clamp(FMath::FloorToInt((V+1.)*.5*Count),0,Count-1);
        // A normalized cube face is narrowest at its corners. This conservative
        // radius also covers cells remapped across either adjacent face.
        const int32 Reach=FMath::Clamp(FMath::CeilToInt(Range/(Radius*Span*.42))+2,1,16);
        const FVector OnGround=Voyager::SurfacePoint(System,Planet,Up);
        for(int32 Y=-Reach;Y<=Reach;++Y)for(int32 X=-Reach;X<=Reach;++X)
        {
            const FVector D=CubeDirection(Face,-1.+(CX+X+.5)*Span,-1.+(CY+Y+.5)*Span);
            const int32 F=DominantFace(D);double DU=0,DV=0;FaceUV(D,F,DU,DV);
            const int32 DX=FMath::Clamp(FMath::FloorToInt((DU+1.)*.5*Count),0,Count-1);
            const int32 DY=FMath::Clamp(FMath::FloorToInt((DV+1.)*.5*Count),0,Count-1);
            const uint64 K=CellKey(Planet,F,Level,DX,DY);
            const double Distance=FVector::DistSquared(OnGround,Voyager::SurfacePoint(System,Planet,CellDirection(K)));
            // Include a cell's full extent to avoid a moving circular cutout in
            // deterministic placements near the streaming boundary.
            if(Distance>FMath::Square(Range+Radius*Span*.8))continue;
            if(double* Existing=Out.Find(K))*Existing=FMath::Min(*Existing,Distance);
            else Out.Add(K,Distance);
        }
    }
    TArray<uint64> OrderedKeys(const TMap<uint64,double>& Distances,int32 Limit)
    {
        TArray<uint64> Keys;Distances.GetKeys(Keys);
        Keys.Sort([&Distances](uint64 A,uint64 B)
        {const double DA=Distances.FindChecked(A),DB=Distances.FindChecked(B);return DA==DB?A<B:DA<DB;});
        if(Keys.Num()>Limit)Keys.SetNum(Limit);
        return Keys;
    }
}

AVoyagerVegetation::AVoyagerVegetation()
{
    PrimaryActorTick.bCanEverTick=true;PrimaryActorTick.TickInterval=.12f;
    bReplicates=false;SetReplicateMovement(false);
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("NaturalVegetationRoot")));
    GetRootComponent()->SetMobility(EComponentMobility::Static);
    Tags.Add(TEXT("VoyagerNaturalVegetation"));
}

void AVoyagerVegetation::BeginPlay()
{
    Super::BeginPlay();
    if(GetNetMode()!=NM_DedicatedServer)LoadNatureMeshes();
    Evaluate(true);
}

void AVoyagerVegetation::LoadNatureMeshes()
{
    if(bLoadedMeshes)return;
    bLoadedMeshes=true;
    static const TCHAR* Names[]={TEXT("SM_Broadleaf"),TEXT("SM_Conifer"),TEXT("SM_DryShrub"),TEXT("SM_Grass"),TEXT("SM_Fern"),TEXT("SM_Boulder")};
    NatureMeshes.SetNum(MeshCount);
    int32 Loaded=0;
    for(int32 I=0;I<MeshCount;++I)
    {
        NatureMeshes[I]=LoadObject<UStaticMesh>(nullptr,*FString::Printf(TEXT("/Game/Nature/Meshes/%s.%s"),Names[I],Names[I]));
        if(NatureMeshes[I])++Loaded;
        else UE_LOG(LogTemp,Warning,TEXT("VOYAGER NATURE missing mesh %s; this species will be skipped"),Names[I]);
    }
    UE_LOG(LogTemp,Display,TEXT("VOYAGER NATURE assets=%d/%d cells_max=%d trunk_bodies_per_view=%d"),Loaded,MeshCount,MaxCanopyCells+MaxGroundCells,MaxTreesPerView);
}

void AVoyagerVegetation::RefreshNow() { Evaluate(true); }

void AVoyagerVegetation::Evaluate(bool bForce)
{
    const auto* State=GetWorld()->GetGameState<AVoyagerState>();
    if(!State)return;
    if(BuiltSystem!=State->SystemSeed)
    {ClearAll();BuiltSystem=State->SystemSeed;bForce=true;}
    if(State->bTransitioning)return;
    TArray<FVector> RenderViews,CollisionViews;
    const bool bAuthorityWorld=GetNetMode()!=NM_Client;
    for(FConstPlayerControllerIterator It=GetWorld()->GetPlayerControllerIterator();It;++It)
    {
        const auto* PC=It->Get();const APawn* Pawn=PC?PC->GetPawn():nullptr;
        if(!Pawn)continue;
        const FVector Position=Pawn->GetActorLocation();
        const int32 Planet=Voyager::NearestPlanet(BuiltSystem,Position);
        const double Altitude=Voyager::SurfaceAltitude(BuiltSystem,Planet,Position);
        if(Altitude>CanopyAltitude||Altitude<-2000.)continue;
        if(PC->IsLocalController()&&GetNetMode()!=NM_DedicatedServer&&RenderViews.Num()<MaxPlayerViews)RenderViews.Add(Position);
        if((bAuthorityWorld||PC->IsLocalController())&&Cast<ACharacter>(Pawn)&&CollisionViews.Num()<MaxPlayerViews)
            CollisionViews.Add(Position);
    }
    if(bForce||ViewMoved(RenderViews,LastRenderViews,1800.))
    {EvaluateDecoration(RenderViews);LastRenderViews=RenderViews;}
    if(bForce||ViewMoved(CollisionViews,LastCollisionViews,700.))
    {EvaluateCollision(CollisionViews);LastCollisionViews=CollisionViews;}
    PruneCache();
}

void AVoyagerVegetation::EvaluateDecoration(const TArray<FVector>& Views)
{
    TMap<uint64,double> Canopy,Ground;
    for(FVector View:Views)
    {
        NearbyCells(BuiltSystem,View,CanopyLevel,CanopyRange,Canopy);
        const int32 Planet=Voyager::NearestPlanet(BuiltSystem,View);
        if(Voyager::SurfaceAltitude(BuiltSystem,Planet,View)<GroundAltitude)
            NearbyCells(BuiltSystem,View,GroundLevel,GroundRange,Ground);
    }
    const TArray<uint64> Far=OrderedKeys(Canopy,MaxCanopyCells),Near=OrderedKeys(Ground,MaxGroundCells);
    DesiredCells.Reset();
    // Interleave the queues so nearby ground cover never waits behind an entire
    // horizon of tree cells after landing.
    for(int32 I=0;I<FMath::Max(Far.Num(),Near.Num());++I)
    {if(Near.IsValidIndex(I))DesiredCells.Add(Near[I]);if(Far.IsValidIndex(I))DesiredCells.Add(Far[I]);}
    WantedCellKeys.Reset();for(uint64 K:DesiredCells)WantedCellKeys.Add(K);
    for(auto& Pair:Cells)
    {
        const bool bVisible=WantedCellKeys.Contains(Pair.Key);
        if(Pair.Value.bVisible==bVisible)continue;
        Pair.Value.bVisible=bVisible;
        for(UHierarchicalInstancedStaticMeshComponent* Component:Pair.Value.Components)
            if(IsValid(Component))Component->SetVisibility(bVisible);
    }
}

TArray<FVoyagerNaturePlacement> AVoyagerVegetation::GeneratePlacements(uint64 K) const
{
    TArray<FVoyagerNaturePlacement> Result;
    const bool bGround=LevelOf(K)==GroundLevel;
    const int32 Planet=PlanetOf(K),Face=FaceOf(K),Biome=Voyager::Biome(BuiltSystem,Planet);
    const double Radius=Voyager::PlanetRadius(BuiltSystem,Planet),Span=2./double(1<<LevelOf(K));
    const double U0=-1.+XOf(K)*Span,V0=-1.+YOf(K)*Span;
    const FVector A=CubeDirection(Face,U0,V0)*Radius,B=CubeDirection(Face,U0+Span,V0)*Radius,C=CubeDirection(Face,U0,V0+Span)*Radius;
    const double Area=FVector::CrossProduct(B-A,C-A).Size();
    const int32 Candidates=FMath::Clamp(FMath::RoundToInt(Area/(bGround?40000.:1800000.)),bGround?24:8,bGround?384:128);
    const int32 Side=FMath::CeilToInt(FMath::Sqrt(double(Candidates)));
    const uint32 Seed=Voyager::Hash(uint32(Voyager::PlanetSeed(BuiltSystem,Planet))^uint32(K)^uint32(K>>32)^0x3854b61du);
    FRandomStream Random{int32(Seed)};
    const uint32 ClimateSeed=Voyager::Hash(uint32(Voyager::PlanetSeed(BuiltSystem,Planet))^0x1491c32bu);
    const FVector ClimateOffset(double(ClimateSeed%77),double(ClimateSeed%41),double(ClimateSeed%103));
    Result.Reserve(Candidates);
    for(int32 I=0;I<Candidates;++I)
    {
        const double U=U0+Span*(double(I%Side)+Random.FRandRange(.08f,.92f))/Side;
        const double V=V0+Span*(double(I/Side)+Random.FRandRange(.08f,.92f))/Side;
        const FVector Up=CubeDirection(Face,U,V);
        const double NorthDistance=(Up-FVector::UpVector).Size()*Radius;
        const double Clearing=bGround?1200.:3500.;
        if(NorthDistance<Clearing)continue;
        const double Feather=bGround?800.:2000.;
        if(NorthDistance<Clearing+Feather&&Random.FRand()>(NorthDistance-Clearing)/Feather)continue;
        if(AVoyagerSettlement::IsWithinSite(BuiltSystem,Planet,Up,1200.))continue;
        if((Up-FVector(2100.,900.,Radius).GetSafeNormal()).Size()*Radius<(bGround?750.:1800.))continue;
        const double Height=Voyager::TerrainHeight(BuiltSystem,Planet,Up);
        if((Biome==0||Biome==4)&&Height<WetBasinLevel)continue;
        const double Habitat=.5+.5*FMath::PerlinNoise3D(Up*(Radius/140000.)+ClimateOffset);
        int32 Mesh=Boulder;
        const float Roll=Random.FRand();
        if(bGround)
        {
            if(Biome==3&&Roll>.08f)continue;
            if(Biome==1&&Roll>.27f)continue;
            if(Biome==2&&Roll>.43f)continue;
            if(Random.FRand()>FMath::Clamp(Habitat+.36,.2,.97))continue;
            Mesh=((Biome==0||Biome==4)&&Random.FRand()<(Biome==4?.23f:.09f))?Fern:Grass;
            if(Biome==1&&Random.FRand()<.12f)Mesh=DryShrub;
        }
        else
        {
            if(Random.FRand()>FMath::Clamp(Habitat+.27,.20,.95))continue;
            switch(Biome)
            {
            case 0:Mesh=Roll<.61f?Broadleaf:Roll<.77f?Conifer:Roll<.88f?DryShrub:Boulder;break;
            case 1:Mesh=Roll<.48f?DryShrub:Boulder;break;
            case 2:Mesh=Roll<.65f?Conifer:Roll<.90f?Boulder:DryShrub;break;
            case 3:Mesh=Roll<.12f?DryShrub:Boulder;break;
            default:Mesh=Roll<.56f?Broadleaf:Roll<.78f?DryShrub:Boulder;break;
            }
        }
        const FQuat Tangent=Voyager::TangentRotation(Up).Quaternion();
        const FVector Position=Voyager::PlanetCenter(BuiltSystem,Planet)+Up*(Radius+Height);
        const FVector PX=Voyager::SurfacePoint(BuiltSystem,Planet,(Up+Tangent.GetAxisX()*200./Radius).GetSafeNormal());
        const FVector PY=Voyager::SurfacePoint(BuiltSystem,Planet,(Up+Tangent.GetAxisY()*200./Radius).GetSafeNormal());
        const FVector Normal=FVector::CrossProduct(PX-Position,PY-Position).GetSafeNormal(UE_SMALL_NUMBER,Up);
        const double Slope=FVector::DotProduct(Normal,Up);
        if(Slope<(Mesh==Broadleaf||Mesh==Conifer?.84:Mesh==Boulder?.62:.74))continue;
        const float Size=Random.FRandRange(bGround?.65f:.65f,bGround?1.45f:1.45f);
        const float Yaw=Random.FRandRange(0.f,360.f);
        FVoyagerNaturePlacement Placement;
        // Near placements reserve two low bits for satellite grass clumps.
        // Coarse obstacle identities are unchanged; their level13 encoding has
        // bit45 set, whereas the level15 near encoding cannot set that bit.
        Placement.Id=bGround?(K<<11)|(uint64(I)<<2):(K<<9)|uint64(I);
        Placement.Mesh=Mesh;
        // Trees remain radial; low vegetation follows the sampled ground normal.
        const FVector LocalUp=Mesh==Broadleaf||Mesh==Conifer?Up:Normal;
        Placement.Rotation=Voyager::TangentRotation(LocalUp).Quaternion()*FRotator(0,Yaw,0).Quaternion();
        Placement.Scale=FVector(Size*Random.FRandRange(.88f,1.13f),Size*Random.FRandRange(.88f,1.13f),Size);
        if(Mesh==Boulder)Placement.Scale*=Random.FRandRange(.42f,1.45f);
        if(Mesh==DryShrub&&bGround)Placement.Scale*=.55;
        Placement.Position=Position-Up*(Mesh==Boulder?14.:2.);
        Result.Add(Placement);
    }
    if(bGround&&(Biome==0||Biome==4)&&!Result.IsEmpty())
    {
        // Photographed clumps read as isolated tufts at two-metre spacing. Keep
        // the stable base distribution and fill its neighbourhood with up to
        // three varied satellites, rather than increasing the streaming radius.
        // Each round visits every base before adding another satellite, and the
        // hard per-cell cap bounds a fully populated64-cell meadow at49,152.
        const int32 BaseCount=Result.Num();
        Result.Reserve(MaxGroundInstancesPerCell);
        for(int32 Round=1;Round<=3&&Result.Num()<MaxGroundInstancesPerCell;++Round)
            for(int32 Index=0;Index<BaseCount&&Result.Num()<MaxGroundInstancesPerCell;++Index)
            {
                const FVoyagerNaturePlacement Base=Result[Index];
                if(Base.Mesh!=Grass)continue;
                FRandomStream ClusterRandom(int32(Voyager::Hash(Seed^uint32(Base.Id)^uint32(Base.Id>>32)^uint32(Round*7307))));
                const double Angle=(Round-1)*UE_TWO_PI/3.+ClusterRandom.FRandRange(-.55f,.55f);
                const double Reach=ClusterRandom.FRandRange(42.f,85.f);
                const FVector Offset=(Base.Rotation.GetAxisX()*FMath::Cos(Angle)+Base.Rotation.GetAxisY()*FMath::Sin(Angle))*Reach;
                const FVector Up=Voyager::SurfaceNormal(BuiltSystem,Planet,Base.Position+Offset);
                if((Up-FVector::UpVector).Size()*Radius<1200.)continue;
                if(AVoyagerSettlement::IsWithinSite(BuiltSystem,Planet,Up,1200.))continue;
                if((Up-FVector(2100.,900.,Radius).GetSafeNormal()).Size()*Radius<750.)continue;
                const double Height=Voyager::TerrainHeight(BuiltSystem,Planet,Up);
                if(Height<WetBasinLevel)continue;
                FVoyagerNaturePlacement Satellite=Base;
                Satellite.Id=Base.Id|uint64(Round);
                Satellite.Position=Voyager::PlanetCenter(BuiltSystem,Planet)+Up*(Radius+Height-2.);
                Satellite.Rotation=Base.Rotation*FRotator(0,ClusterRandom.FRandRange(-95.f,95.f),0).Quaternion();
                Satellite.Scale*=ClusterRandom.FRandRange(.86f,1.10f);
                Result.Add(Satellite);
            }
    }
    return Result;
}

const TArray<FVoyagerNaturePlacement>& AVoyagerVegetation::CoarsePlacements(uint64 Key)
{
    if(const auto* Cached=CoarseCache.Find(Key))return *Cached;
    return CoarseCache.Add(Key,GeneratePlacements(Key));
}

void AVoyagerVegetation::BuildCell(uint64 Key)
{
    if(Cells.Contains(Key)||Cells.Num()>=MaxCanopyCells+MaxGroundCells||GetNetMode()==NM_DedicatedServer)return;
    const bool bGround=LevelOf(Key)==GroundLevel;
    const TArray<FVoyagerNaturePlacement> Placements=bGround?GeneratePlacements(Key):CoarsePlacements(Key);
    const FVector Origin=Voyager::SurfacePoint(BuiltSystem,PlanetOf(Key),CellDirection(Key));
    FVoyagerVegetationCell& Cell=Cells.Add(Key);
    TArray<FTransform> Batches[MeshCount];
    for(const auto& Placement:Placements)
    {
        if(!NatureMeshes.IsValidIndex(Placement.Mesh)||!NatureMeshes[Placement.Mesh])continue;
        Batches[Placement.Mesh].Emplace(Placement.Rotation,Placement.Position-Origin,Placement.Scale);
        ++Cell.Instances;
        if(Placement.Mesh==Broadleaf||Placement.Mesh==Conifer)++Cell.Trees;
        if(Placement.Mesh==Grass||Placement.Mesh==Fern)++Cell.Grass;
    }
    for(int32 Mesh=0;Mesh<MeshCount;++Mesh)
    {
        if(Batches[Mesh].IsEmpty())continue;
        auto* Instances=NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
        AddInstanceComponent(Instances);Instances->SetupAttachment(GetRootComponent());
        Instances->SetRelativeLocation(Origin);Instances->SetMobility(EComponentMobility::Static);
        Instances->SetStaticMesh(NatureMeshes[Mesh]);
        const int32 Biome=Voyager::Biome(BuiltSystem,PlanetOf(Key));
        // Reuse one material per species/climate. Only leaf slots are tinted on
        // combined tree meshes, so trunks retain their photographed bark color.
        if(Biome!=0||Mesh==Boulder)
        {
            const int32 VariantKey=Biome*MeshCount+Mesh;
            const int32 Slot=Mesh==Broadleaf||Mesh==Conifer||Mesh==DryShrub?1:0;
            if(!MaterialVariants.Contains(VariantKey))
                if(UMaterialInterface* Base=NatureMeshes[Mesh]->GetMaterial(Slot))
                {
                    static const FLinearColor LeafTints[]={FLinearColor(1,1,1),FLinearColor(.94f,.78f,.42f),FLinearColor(.72f,.85f,.80f),FLinearColor(.48f,.33f,.20f),FLinearColor(.88f,.69f,1.f)};
                    static const FLinearColor RockTints[]={FLinearColor(.78f,.87f,.77f),FLinearColor(.93f,.75f,.54f),FLinearColor(.88f,.94f,1.02f),FLinearColor(.45f,.39f,.35f),FLinearColor(.80f,.69f,.91f)};
                    FLinearColor Color=Mesh==Boulder?RockTints[Biome]:LeafTints[Biome];
                    if(Mesh==DryShrub)Color*=FLinearColor(.83f,.64f,.34f);
                    if(Mesh==Conifer)Color*=FLinearColor(.90f,1.02f,.83f);
                    auto* Material=UMaterialInstanceDynamic::Create(Base,this);
                    Material->SetVectorParameterValue(TEXT("Tint"),Color);MaterialVariants.Add(VariantKey,Material);
                }
            if(auto* Material=MaterialVariants.Find(VariantKey))Instances->SetMaterial(Slot,Material->Get());
        }
        Instances->SetCollisionEnabled(ECollisionEnabled::NoCollision);Instances->SetGenerateOverlapEvents(false);
        Instances->SetCanEverAffectNavigation(false);Instances->SetCastShadow(!bGround);
        Instances->SetCullDistances(bGround?6500:44000,bGround?12000:75000);
        Instances->bAutoRebuildTreeOnInstanceChanges=false;
        Instances->RegisterComponent();
        Instances->AddInstances(Batches[Mesh],false,false,false);
        Instances->BuildTreeIfOutdated(true,true);
        Cell.Components.Add(Instances);
    }
}

void AVoyagerVegetation::EvaluateCollision(const TArray<FVector>& Views)
{
    WantedTrunks.Reset();DesiredTrunks.Reset();CollisionCellKeys.Reset();
    TMap<uint64,double> GlobalDistance;
    for(FVector View:Views)
    {
        TMap<uint64,double> Nearby;NearbyCells(BuiltSystem,View,CanopyLevel,TrunkRange,Nearby);
        TMap<uint64,double> TreeDistance;
        TMap<uint64,FVoyagerNaturePlacement> Samples;
        for(const auto& Pair:Nearby)
        {
            CollisionCellKeys.Add(Pair.Key);
            for(const auto& Placement:CoarsePlacements(Pair.Key))
            {
                if(Placement.Mesh!=Broadleaf&&Placement.Mesh!=Conifer&&Placement.Mesh!=Boulder)continue;
                const double Distance=FVector::DistSquared(View,Placement.Position);
                if(Distance>TrunkRange*TrunkRange)continue;
                TreeDistance.Add(Placement.Id,Distance);Samples.Add(Placement.Id,Placement);
            }
        }
        const TArray<uint64> Nearest=OrderedKeys(TreeDistance,MaxTreesPerView);
        for(uint64 Id:Nearest)
        {
            WantedTrunks.Add(Id,Samples.FindChecked(Id));
            const double Distance=TreeDistance.FindChecked(Id);
            if(double* Existing=GlobalDistance.Find(Id))*Existing=FMath::Min(*Existing,Distance);
            else GlobalDistance.Add(Id,Distance);
        }
    }
    DesiredTrunks=OrderedKeys(GlobalDistance,MaxTreesPerView*MaxPlayerViews);
    for(auto& Pair:TrunkBodies)
        if(IsValid(Pair.Value))
        {
            const ECollisionEnabled::Type Enabled=WantedTrunks.Contains(Pair.Key)?ECollisionEnabled::QueryOnly:ECollisionEnabled::NoCollision;
            if(Pair.Value->GetCollisionEnabled()!=Enabled)Pair.Value->SetCollisionEnabled(Enabled);
        }
}

void AVoyagerVegetation::BuildTrunk(uint64 Id)
{
    if(TrunkBodies.Contains(Id)||TrunkBodies.Num()>=MaxTreesPerView*MaxPlayerViews)return;
    const auto* Placement=WantedTrunks.Find(Id);if(!Placement)return;
    // Dedicated servers do not load appearance assets. Every peer uses the
    // same small physical obstacle, independent of the visual mesh's active LOD.
    const bool bRock=Placement->Mesh==Boulder;
    const float Height=(bRock?180.f:Placement->Mesh==Conifer?760.f:470.f)*float(Placement->Scale.Z);
    const float RawRadius=(bRock?105.f:Placement->Mesh==Conifer?24.f:27.f)*float(FMath::Max(Placement->Scale.X,Placement->Scale.Y));
    const float Radius=FMath::Min(RawRadius,Height*.5f);
    auto* Capsule=NewObject<UCapsuleComponent>(this);
    AddInstanceComponent(Capsule);Capsule->SetupAttachment(GetRootComponent());
    Capsule->SetMobility(EComponentMobility::Static);
    Capsule->InitCapsuleSize(Radius,Height*.5f);
    Capsule->SetRelativeLocationAndRotation(Placement->Position+Placement->Rotation.GetAxisZ()*Height*.5,Placement->Rotation);
    Capsule->SetCollisionEnabled(ECollisionEnabled::QueryOnly);Capsule->SetCollisionObjectType(ECC_WorldStatic);
    Capsule->SetCollisionResponseToAllChannels(ECR_Ignore);Capsule->SetCollisionResponseToChannel(ECC_Pawn,ECR_Block);
    Capsule->SetGenerateOverlapEvents(false);Capsule->SetCanEverAffectNavigation(false);
    Capsule->CanCharacterStepUpOn=ECB_No;
    Capsule->SetWalkableSlopeOverride(FWalkableSlopeOverride(WalkableSlope_Unwalkable,0.f));
    Capsule->SetHiddenInGame(true);Capsule->RegisterComponent();TrunkBodies.Add(Id,Capsule);
}

void AVoyagerVegetation::RemoveCell(uint64 Key)
{
    if(auto* Cell=Cells.Find(Key))for(UHierarchicalInstancedStaticMeshComponent* Component:Cell->Components)
        if(IsValid(Component)){RemoveInstanceComponent(Component);Component->DestroyComponent();}
    Cells.Remove(Key);
}
void AVoyagerVegetation::RemoveTrunk(uint64 Id)
{
    if(auto* Found=TrunkBodies.Find(Id))if(IsValid(Found->Get()))
    {RemoveInstanceComponent(Found->Get());(*Found)->DestroyComponent();}
    TrunkBodies.Remove(Id);
}
void AVoyagerVegetation::ClearAll()
{
    TArray<uint64> Keys;Cells.GetKeys(Keys);for(uint64 K:Keys)RemoveCell(K);
    TrunkBodies.GetKeys(Keys);for(uint64 K:Keys)RemoveTrunk(K);
    CoarseCache.Reset();WantedTrunks.Reset();DesiredCells.Reset();DesiredTrunks.Reset();WantedCellKeys.Reset();CollisionCellKeys.Reset();
    LastRenderViews.Reset();LastCollisionViews.Reset();
}
void AVoyagerVegetation::PruneCache()
{
    TSet<uint64> Keep=CollisionCellKeys;
    for(uint64 Key:DesiredCells)if(LevelOf(Key)==CanopyLevel)Keep.Add(Key);
    TArray<uint64> Keys;CoarseCache.GetKeys(Keys);
    for(uint64 K:Keys)if(!Keep.Contains(K))CoarseCache.Remove(K);
}

void AVoyagerVegetation::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);Evaluate(false);
    // Retirement is amortized too: a continuous climb out of the atmosphere
    // must not destroy hundreds of HISM components in one frame. Visibility and
    // collision were already disabled when the desired set changed. Re-check
    // that set here, because a cell can become wanted again before retirement.
    TArray<uint64> Retiring;Cells.GetKeys(Retiring);
    int32 Removed=0;
    for(uint64 Key:Retiring)if(!WantedCellKeys.Contains(Key))
    {RemoveCell(Key);if(++Removed>=8)break;}
    TrunkBodies.GetKeys(Retiring);Removed=0;
    for(uint64 Id:Retiring)if(!WantedTrunks.Contains(Id))
    {RemoveTrunk(Id);if(++Removed>=24)break;}
    int32 Created=0;
    for(uint64 Key:DesiredCells)if(!Cells.Contains(Key))
    {BuildCell(Key);if(++Created>=2)break;}
    Created=0;
    for(uint64 Id:DesiredTrunks)if(!TrunkBodies.Contains(Id))
    {BuildTrunk(Id);if(++Created>=12)break;}
}
void AVoyagerVegetation::EndPlay(const EEndPlayReason::Type Reason)
{ClearAll();Super::EndPlay(Reason);}
int32 AVoyagerVegetation::ActiveInstanceCount() const
{int32 Total=0;for(const auto& Pair:Cells)Total+=Pair.Value.Instances;return Total;}
int32 AVoyagerVegetation::ActiveTreeCount() const
{int32 Total=0;for(const auto& Pair:Cells)Total+=Pair.Value.Trees;return Total;}
int32 AVoyagerVegetation::ActiveGrassCount() const
{int32 Total=0;for(const auto& Pair:Cells)Total+=Pair.Value.Grass;return Total;}
