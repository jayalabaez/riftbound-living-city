#include "VoyagerWorld.h"
#include "VoyagerData.h"
#include "VoyagerGameMode.h"
#include "VoyagerSettlement.h"
#include "VoyagerWildlife.h"
#include "VoyagerCitizen.h"
#include "VoyagerVegetation.h"
#include "VoyagerCosmos.h"
#include "RiftVisual.h"
#include "ProceduralMeshComponent.h"
#include "Async/Async.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    constexpr int32 PatchResolution = 32;
    constexpr int32 MaxPatchLevel = 18;
    constexpr int32 TargetPatchNodes = 350;
    constexpr int32 MaxResidentPatches = 1500;
    constexpr int32 MaxMeshJobs = 4;
    constexpr int32 DetailLevel = 14;
    constexpr int32 MaxDetailCells = 100;
    constexpr int32 MaxResources = 80;
    constexpr uint64 CoordinateMask = (1ull << 18) - 1;

    uint64 PatchKey(int32 Planet, int32 Face, int32 Level, int32 X, int32 Y)
    {
        return uint64(Y) | (uint64(X) << 18) | (uint64(Level) << 36) |
            (uint64(Face) << 41) | (uint64(Planet) << 44);
    }
    int32 PatchPlanet(uint64 K) { return int32((K >> 44) & 7); }
    int32 PatchFace(uint64 K) { return int32((K >> 41) & 7); }
    int32 PatchLevel(uint64 K) { return int32((K >> 36) & 31); }
    int32 PatchX(uint64 K) { return int32((K >> 18) & CoordinateMask); }
    int32 PatchY(uint64 K) { return int32(K & CoordinateMask); }
    uint64 Child(uint64 K, int32 I)
    {
        return PatchKey(PatchPlanet(K), PatchFace(K), PatchLevel(K) + 1,
            PatchX(K) * 2 + (I & 1), PatchY(K) * 2 + (I >> 1));
    }
    uint64 Parent(uint64 K)
    {
        return PatchKey(PatchPlanet(K), PatchFace(K), PatchLevel(K) - 1, PatchX(K) / 2, PatchY(K) / 2);
    }
    FVector CubeDirection(int32 Face, double U, double V)
    {
        switch (Face)
        {
        case 0: return FVector(1, U, V).GetSafeNormal();
        case 1: return FVector(-1, -U, V).GetSafeNormal();
        case 2: return FVector(-U, 1, V).GetSafeNormal();
        case 3: return FVector(U, -1, V).GetSafeNormal();
        case 4: return FVector(U, V, 1).GetSafeNormal();
        default: return FVector(U, -V, -1).GetSafeNormal();
        }
    }
    bool FaceUV(FVector Direction, int32 Face, double& U, double& V)
    {
        double D = 0;
        switch (Face)
        {
        case 0: D = Direction.X; U = Direction.Y; V = Direction.Z; break;
        case 1: D = -Direction.X; U = -Direction.Y; V = Direction.Z; break;
        case 2: D = Direction.Y; U = -Direction.X; V = Direction.Z; break;
        case 3: D = -Direction.Y; U = Direction.X; V = Direction.Z; break;
        case 4: D = Direction.Z; U = Direction.X; V = Direction.Y; break;
        default: D = -Direction.Z; U = Direction.X; V = -Direction.Y; break;
        }
        if (D <= 1.e-10) return false;
        U /= D; V /= D; return true;
    }
    int32 DominantFace(FVector Direction)
    {
        const FVector A = Direction.GetAbs();
        if (A.X >= A.Y && A.X >= A.Z) return Direction.X >= 0 ? 0 : 1;
        if (A.Y >= A.Z) return Direction.Y >= 0 ? 2 : 3;
        return Direction.Z >= 0 ? 4 : 5;
    }
    FVector PatchDirection(uint64 K)
    {
        const double N = double(1 << PatchLevel(K));
        return CubeDirection(PatchFace(K), -1.0 + (PatchX(K) + .5) * 2.0 / N,
            -1.0 + (PatchY(K) + .5) * 2.0 / N);
    }
    double PatchWidth(int32 System, uint64 K)
    {
        return Voyager::PlanetRadius(System, PatchPlanet(K)) * 2.0 / double(1 << PatchLevel(K));
    }
    double PatchDistance(int32 System, uint64 K, const TArray<FVector>& Views)
    {
        const int32 P = PatchPlanet(K), F = PatchFace(K);
        const double N = double(1 << PatchLevel(K));
        const double U0 = -1.0 + PatchX(K) * 2.0 / N, U1 = U0 + 2.0 / N;
        const double V0 = -1.0 + PatchY(K) * 2.0 / N, V1 = V0 + 2.0 / N;
        double Best = TNumericLimits<double>::Max();
        for (FVector View : Views)
        {
            double U = 0, V = 0;
            const FVector Up = Voyager::SurfaceNormal(System, P, View);
            if (!FaceUV(Up, F, U, V))
            {
                Best = FMath::Min(Best, FVector::Dist(View, Voyager::SurfacePoint(System, P, PatchDirection(K))));
                continue;
            }
            const FVector Closest = CubeDirection(F, FMath::Clamp(U, U0, U1), FMath::Clamp(V, V0, V1));
            Best = FMath::Min(Best, FVector::Dist(View, Voyager::SurfacePoint(System, P, Closest)));
        }
        return Best;
    }

    template<typename T> T* Component(AActor* Owner, const FString& Name,
        TArray<TObjectPtr<UActorComponent>>& Storage)
    {
        T* Value = NewObject<T>(Owner, FName(Name));
        Value->SetNetAddressable();
        Owner->AddInstanceComponent(Value);
        Value->SetupAttachment(Owner->GetRootComponent());
        Storage.Add(Value);
        return Value;
    }
    void Dispose(AActor* Owner, TArray<TObjectPtr<UActorComponent>>& Components)
    {
        for (UActorComponent* Value : Components)
        {
            if (!IsValid(Value)) continue;
            Owner->RemoveInstanceComponent(Value);
            Value->DestroyComponent();
            Value->SetFlags(RF_Transient);
            const FName Name = MakeUniqueObjectName(GetTransientPackage(), Value->GetClass(), TEXT("RetiredVoyagerComponent"));
            Value->Rename(*Name.ToString(), GetTransientPackage(),
                REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty | REN_SkipComponentRegWork);
        }
        Components.Empty();
    }
    UStaticMeshComponent* Part(AActor* Owner, TArray<TObjectPtr<UActorComponent>>& Storage,
        const FString& Name, const TCHAR* Shape, FVector Location, FVector Scale,
        FLinearColor Tint, bool bGlow = false, FRotator Rotation = FRotator::ZeroRotator,
        bool bCollision = false)
    {
        auto* Mesh = Component<UStaticMeshComponent>(Owner, Name, Storage);
        Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,
            *FString::Printf(TEXT("/Engine/BasicShapes/%s.%s"), Shape, Shape)));
        Mesh->SetRelativeLocation(Location);
        Mesh->SetRelativeScale3D(Scale);
        Mesh->SetRelativeRotation(Rotation);
        Mesh->SetMaterial(0, RiftVisual::Material(Owner, Tint, bGlow));
        Mesh->SetCollisionProfileName(bCollision ? TEXT("BlockAll") : TEXT("NoCollision"));
        Mesh->SetCollisionObjectType(ECC_WorldStatic);
        Mesh->SetCanEverAffectNavigation(false);
        Mesh->SetCastShadow(!bGlow);
        Mesh->SetMobility(EComponentMobility::Static);
        Mesh->RegisterComponent();
        return Mesh;
    }
    UInstancedStaticMeshComponent* Instances(AActor* Owner,
        TArray<TObjectPtr<UActorComponent>>& Storage, const FString& Name, const TCHAR* Shape,
        FLinearColor Tint, FVector Origin, bool bGlow = false, bool bMovable = false)
    {
        auto* Mesh = Component<UInstancedStaticMeshComponent>(Owner, Name, Storage);
        Mesh->SetRelativeLocation(Origin);
        Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,
            *FString::Printf(TEXT("/Engine/BasicShapes/%s.%s"), Shape, Shape)));
        Mesh->SetMaterial(0, RiftVisual::Material(Owner, Tint, bGlow));
        Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Mesh->SetCanEverAffectNavigation(false);
        Mesh->SetCastShadow(!bGlow);
        Mesh->SetMobility(bMovable ? EComponentMobility::Movable : EComponentMobility::Static);
        Mesh->RegisterComponent();
        return Mesh;
    }
    void Instance(UInstancedStaticMeshComponent* Mesh, FVector Location, FVector Scale,
        FRotator Rotation = FRotator::ZeroRotator)
    {
        Mesh->AddInstance(FTransform(Rotation, Location, Scale));
    }

    FVoyagerPatchMesh GeneratePatch(int32 System, uint64 K)
    {
        FVoyagerPatchMesh Result;
        Result.Key = K;
        const int32 P = PatchPlanet(K), F = PatchFace(K), L = PatchLevel(K);
        // Coast intersections were visibly triangular when an entire orbital
        // patch sampled the radial field only 32 times. Keep ground collision
        // unchanged while sampling distant silhouettes/coastlines more densely.
        const int32 N = L<=5?96:PatchResolution, Stride = N + 1;
        const double Cells = double(1 << L), Span = 2.0 / Cells;
        const double U0 = -1.0 + PatchX(K) * Span, V0 = -1.0 + PatchY(K) * Span;
        Result.Origin = Voyager::SurfacePoint(System, P, PatchDirection(K));
        Result.bCollision = L >= 12;
        Result.Vertices.Reserve(Stride * Stride + 4 * Stride);
        Result.UVs.Reserve(Stride * Stride + 4 * Stride);
        for (int32 Y = 0; Y <= N; ++Y)
            for (int32 X = 0; X <= N; ++X)
            {
                const FVector Up = CubeDirection(F, U0 + Span * X / N, V0 + Span * Y / N);
                Result.Vertices.Add(Voyager::SurfacePoint(System, P, Up) - Result.Origin);
                Result.UVs.Add(FVector2D(double(X) / N, double(Y) / N));
            }
        Result.Normals.SetNum(Stride * Stride);
        const FVector Center = Voyager::PlanetCenter(System, P);
        for (int32 Y = 0; Y <= N; ++Y)
            for (int32 X = 0; X <= N; ++X)
            {
                const int32 I = Y * Stride + X;
                const FVector DX = Result.Vertices[Y * Stride + FMath::Min(X + 1, N)] - Result.Vertices[Y * Stride + FMath::Max(X - 1, 0)];
                const FVector DY = Result.Vertices[FMath::Min(Y + 1, N) * Stride + X] - Result.Vertices[FMath::Max(Y - 1, 0) * Stride + X];
                FVector Normal = FVector::CrossProduct(DX, DY).GetSafeNormal();
                if (FVector::DotProduct(Normal, Result.Origin + Result.Vertices[I] - Center) < 0) Normal *= -1;
                Result.Normals[I] = Normal;
            }
        for (int32 Y = 0; Y < N; ++Y)
            for (int32 X = 0; X < N; ++X)
            {
                const int32 A = Y * Stride + X, B = A + 1, C = A + Stride, D = C + 1;
                Result.Indices.Append({A, C, B, B, C, D});
            }
        // Radial skirts overlap the neighbouring level, preventing daylight through LOD seams.
        const double SkirtDepth = FMath::Clamp(PatchWidth(System, K) * .035, 100.0, 650000.0);
        for (int32 Edge = 0; Edge < 4; ++Edge)
        {
            int32 PreviousTop = INDEX_NONE, PreviousBottom = INDEX_NONE;
            for (int32 I = 0; I <= N; ++I)
            {
                const int32 Top = Edge == 0 ? I : Edge == 1 ? I * Stride + N : Edge == 2 ? N * Stride + (N - I) : (N - I) * Stride;
                const FVector Up = (Result.Origin + Result.Vertices[Top] - Center).GetSafeNormal();
                const int32 Bottom = Result.Vertices.Add(Result.Vertices[Top] - Up * SkirtDepth);
                Result.Normals.Add(FVector(Result.Normals[Top]));
                Result.UVs.Add(FVector2D(Result.UVs[Top]));
                if (I > 0)
                {
                    Result.Indices.Append({PreviousTop, Top, PreviousBottom, Top, Bottom, PreviousBottom,
                        PreviousTop, PreviousBottom, Top, Top, PreviousBottom, Bottom});
                }
                PreviousTop = Top; PreviousBottom = Bottom;
            }
        }
        return Result;
    }
}

AVoyagerWorld::AVoyagerWorld()
{
    bReplicates = true;
    bAlwaysRelevant = true;
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickInterval = .04f;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("VoyagerWorldRoot")));
    GetRootComponent()->SetMobility(EComponentMobility::Static);
}
void AVoyagerWorld::BeginPlay()
{
    Super::BeginPlay(); RebuildNow();
    Vegetation=GetWorld()->SpawnActor<AVoyagerVegetation>(FVector::ZeroVector,FRotator::ZeroRotator);
    Cosmos=GetWorld()->SpawnActor<AVoyagerCosmos>(FVector::ZeroVector,FRotator::ZeroRotator);
    if(HasAuthority())
    {
        FActorSpawnParameters Params;Params.Owner=this;
        GetWorld()->SpawnActor<AVoyagerSettlement>(FVector::ZeroVector,FRotator::ZeroRotator,Params);
        GetWorld()->SpawnActor<AVoyagerWildlifeManager>(FVector::ZeroVector,FRotator::ZeroRotator,Params);
        GetWorld()->SpawnActor<AVoyagerCitizenManager>(FVector::ZeroVector,FRotator::ZeroRotator,Params);
    }
}
void AVoyagerWorld::EndPlay(const EEndPlayReason::Type Reason)
{
    if(IsValid(Vegetation))Vegetation->Destroy();
    Vegetation=nullptr;
    if(IsValid(Cosmos))Cosmos->Destroy();
    Cosmos=nullptr;
    ClearScene();
    Super::EndPlay(Reason);
}
void AVoyagerWorld::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    const AVoyagerState* State = GetWorld()->GetGameState<AVoyagerState>();
    if (!State) return;
    if (BuiltSystem != State->SystemSeed || BuiltRevision != State->Revision) RebuildNow();
    GatherViews();
    UpdateAtmosphere();
    LODCountdown -= DeltaSeconds;
    if (LODCountdown <= 0.f) { EvaluateLOD(); LODCountdown = .24f; }
    StreamPatches();
    DetailCountdown -= DeltaSeconds;
    if (DetailCountdown <= 0.f) { StreamDetails(); DetailCountdown = .16f; }
}
void AVoyagerWorld::GatherViews()
{
    Views.Reset();
    for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
    {
        const APlayerController* PC = It->Get();
        if (!PC || (!HasAuthority() && !PC->IsLocalController())) continue;
        if (const APawn* Pawn = PC->GetPawn()) Views.Add(Pawn->GetActorLocation());
    }
    if (Views.IsEmpty())
    {
        const AVoyagerState* State = GetWorld()->GetGameState<AVoyagerState>();
        const int32 P = State ? FMath::Clamp(State->PlanetIndex, 0, 4) : 0;
        Views.Add(Voyager::SurfacePoint(FMath::Max(1, BuiltSystem), P, FVector::UpVector, 180.0));
    }
}
void AVoyagerWorld::RebuildNow()
{
    const AVoyagerState* State = GetWorld()->GetGameState<AVoyagerState>();
    if (!State || (BuiltSystem == State->SystemSeed && BuiltRevision == State->Revision)) return;
    ClearScene();
    BuiltSystem = State->SystemSeed;
    BuiltRevision = State->Revision;
    GatherViews();
    UMaterialInterface* Surface = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Nature/Materials/M_NatureTerrain.M_NatureTerrain"));
    if(!Surface)Surface=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Materials/M_VoyagerTerrain.M_VoyagerTerrain"));
    for (int32 P = 0; P < Voyager::PlanetCount; ++P)
    {
        const FLinearColor Tint = Voyager::BiomeColor(Voyager::Biome(BuiltSystem, P));
        UMaterialInstanceDynamic* Material = Surface ? UMaterialInstanceDynamic::Create(Surface, this) : RiftVisual::Material(this, Tint * .72f);
        const FVector Center = Voyager::PlanetCenter(BuiltSystem, P);
        const uint32 Seed = uint32(Voyager::PlanetSeed(BuiltSystem, P));
        Material->SetVectorParameterValue(TEXT("Tint"), Tint * .67f);
        Material->SetVectorParameterValue(TEXT("LandTint"), Tint * 1.28f);
        Material->SetVectorParameterValue(TEXT("PlanetCenter"), FLinearColor(float(Center.X), float(Center.Y), float(Center.Z), 1.f));
        Material->SetScalarParameterValue(TEXT("PlanetRadius"), float(Voyager::PlanetRadius(BuiltSystem, P)));
        static const float SnowCoverage[]={0.f,0.f,.72f,0.f,.025f};
        Material->SetScalarParameterValue(TEXT("SnowCoverage"),SnowCoverage[Voyager::Biome(BuiltSystem,P)]);
        static const FLinearColor FarLand[]={FLinearColor(.045f,.095f,.025f),FLinearColor(.24f,.15f,.07f),
            FLinearColor(.38f,.45f,.5f),FLinearColor(.07f,.045f,.035f),FLinearColor(.10f,.13f,.09f)};
        Material->SetVectorParameterValue(TEXT("FarLandTint"),FarLand[Voyager::Biome(BuiltSystem,P)]);
        Material->SetVectorParameterValue(TEXT("SeedOffset"), FLinearColor(float(Seed % 997) * .117f, float(Voyager::Hash(Seed) % 991) * .131f, float(Voyager::Hash(Seed + 37) % 983) * .109f));
        GroundMaterials.Add(Material);
    }
    BuildLighting();
    BuildBackdrop();
    EvaluateLOD();
    // Startup/system arrival builds collision before possession. Atmospheric flight never rebuilds.
    for (uint64 K : DesiredOrder) InstallPatch(GeneratePatch(BuiltSystem, K));
    UpdatePatchVisibility();
    BuildLandmarks();
    StreamDetails(true);
    UpdateAtmosphere();
    LODCountdown = .24f;
    DetailCountdown = .16f;
}
void AVoyagerWorld::ClearScene()
{
    PendingMeshes.Empty(); // Jobs own only value data; abandoning a result is safe.
    TArray<uint64> Keys;
    Chunks.GetKeys(Keys);
    for (uint64 K : Keys) RemovePatch(K);
    DetailCells.GetKeys(Keys);
    for (uint64 K : Keys) RemoveDetailCell(K);
    Dispose(this, SceneComponents);
    GroundMaterials.Empty();
    Atmosphere = nullptr; Skylight = nullptr; Clouds=nullptr;CloudMaterial=nullptr;
    AtmosphereShells.Empty();CloudShells.Empty(); StarFields.Empty();
    DesiredNodes.Empty(); DesiredOrder.Empty();
    HarvestedResources.Empty(); HarvestedOrder.Empty();
    AtmospherePlanet = INDEX_NONE;
}
float AVoyagerWorld::HeightAt(float X, float Y, int32 Seed)
{
    // Compatibility for old prototype callers. New movement uses Voyager::SurfaceAltitude.
    const FVector2D P(X / 7200.f + float(uint32(Seed) % 17003u) * .113f,
        Y / 7200.f + float(Voyager::Hash(uint32(Seed)) % 19001u) * .127f);
    return FMath::PerlinNoise2D(P) * 940.f;
}
void AVoyagerWorld::EvaluateLOD()
{
    struct FCandidate { uint64 K; double Score; };
    TArray<FCandidate> Leaves;
    DesiredNodes.Reset();
    auto Score = [this](uint64 K)
    {
        if (PatchLevel(K) >= MaxPatchLevel) return 0.0;
        return PatchWidth(BuiltSystem, K) / FMath::Max(80.0, PatchDistance(BuiltSystem, K, Views));
    };
    for (int32 P = 0; P < Voyager::PlanetCount; ++P)
        for (int32 F = 0; F < 6; ++F)
        {
            const uint64 K = PatchKey(P, F, 0, 0, 0);
            DesiredNodes.Add(K); Leaves.Add({K, Score(K)});
        }
    const int32 NodeBudget = TargetPatchNodes * FMath::Clamp(Views.Num(), 1, 4);
    while (DesiredNodes.Num() + 4 <= NodeBudget)
    {
        int32 Best = INDEX_NONE; double BestScore = .85;
        for (int32 I = 0; I < Leaves.Num(); ++I)
            if (Leaves[I].Score > BestScore) { Best = I; BestScore = Leaves[I].Score; }
        if (Best == INDEX_NONE) break;
        const uint64 K = Leaves[Best].K;
        Leaves.RemoveAtSwap(Best);
        for (int32 I = 0; I < 4; ++I)
        {
            const uint64 C = Child(K, I);
            DesiredNodes.Add(C); Leaves.Add({C, Score(C)});
        }
    }
    DesiredOrder = DesiredNodes.Array();
    TMap<uint64, double> Distances;
    for (uint64 K : DesiredOrder) Distances.Add(K, PatchDistance(BuiltSystem, K, Views));
    DesiredOrder.Sort([&Distances](uint64 A, uint64 B)
    {
        const int32 LA = PatchLevel(A), LB = PatchLevel(B);
        if (LA == 0 || LB == 0) return LA == LB ? A < B : LA < LB;
        // Near branches win; an ancestor is always generated before its descendants.
        const double DA = Distances.FindChecked(A), DB = Distances.FindChecked(B);
        if (DA != DB) return DA < DB;
        return LA == LB ? A < B : LA < LB;
    });
    TArray<uint64> Existing;
    Chunks.GetKeys(Existing);
    for (uint64 K : Existing) if (!DesiredNodes.Contains(K)) RemovePatch(K);
    UpdatePatchVisibility();
}
void AVoyagerWorld::InstallPatch(FVoyagerPatchMesh&& Mesh)
{
    if (Chunks.Contains(Mesh.Key) || Chunks.Num() >= MaxResidentPatches) return;
    FVoyagerTerrainChunk& Chunk = Chunks.Add(Mesh.Key);
    Chunk.Origin = Mesh.Origin;
    Chunk.bCollision = Mesh.bCollision;
    Chunk.bVisible = false;
    const FString Name = FString::Printf(TEXT("SpherePatch_%llu_%d"), Mesh.Key, BuiltRevision);
    auto* Terrain = Component<UProceduralMeshComponent>(this, Name, Chunk.Components);
    Chunk.Terrain = Terrain;
    Terrain->SetRelativeLocation(Mesh.Origin);
    // Carry only the fractional 4m tile origin into the shader. Near vertices
    // remain precise even when their planet is thousands of kilometres away.
    const FVector TextureOrigin=(Mesh.Origin-Voyager::PlanetCenter(BuiltSystem,PatchPlanet(Mesh.Key)))/400.0;
    Terrain->SetCustomPrimitiveDataVector3(0,FVector(TextureOrigin.X-FMath::FloorToDouble(TextureOrigin.X),
        TextureOrigin.Y-FMath::FloorToDouble(TextureOrigin.Y),TextureOrigin.Z-FMath::FloorToDouble(TextureOrigin.Z)));
    Terrain->SetCollisionProfileName(TEXT("BlockAll"));
    Terrain->SetCollisionObjectType(ECC_WorldStatic);
    Terrain->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Terrain->SetCanEverAffectNavigation(false);
    Terrain->bUseComplexAsSimpleCollision = true;
    Terrain->bUseAsyncCooking = false;
    Terrain->SetMobility(EComponentMobility::Static);
    Terrain->SetMaterial(0, GroundMaterials[PatchPlanet(Mesh.Key)]);
    Terrain->SetVisibility(false);
    Terrain->RegisterComponent();
    TArray<FLinearColor> Colors;
    TArray<FProcMeshTangent> Tangents;
    Terrain->CreateMeshSection_LinearColor(0, Mesh.Vertices, Mesh.Indices, Mesh.Normals, Mesh.UVs, Colors, Tangents, Mesh.bCollision);
}
void AVoyagerWorld::RemovePatch(uint64 K)
{
    if (FVoyagerTerrainChunk* Chunk = Chunks.Find(K)) Dispose(this, Chunk->Components);
    Chunks.Remove(K);
}
void AVoyagerWorld::StreamPatches()
{
    int32 Installed = 0;
    for (int32 I = PendingMeshes.Num() - 1; I >= 0; --I)
    {
        if (!PendingMeshes[I]->Future.IsReady()) continue;
        FVoyagerPatchMesh Result = PendingMeshes[I]->Future.Get();
        PendingMeshes.RemoveAtSwap(I);
        if (DesiredNodes.Contains(Result.Key)) { InstallPatch(MoveTemp(Result)); ++Installed; }
        if (Installed >= 4) break;
    }
    if (Installed > 0) UpdatePatchVisibility();
    for (uint64 K : DesiredOrder)
    {
        if (PendingMeshes.Num() >= MaxMeshJobs || Chunks.Num() + PendingMeshes.Num() >= MaxResidentPatches) break;
        if (Chunks.Contains(K) || (PatchLevel(K) > 0 && !Chunks.Contains(Parent(K)))) continue;
        bool bPending = false;
        for (const auto& Job : PendingMeshes) if (Job->Key == K) { bPending = true; break; }
        if (bPending) continue;
        auto Job = MakeUnique<FVoyagerPatchJob>();
        Job->Key = K;
        const int32 System = BuiltSystem;
        Job->Future = Async(EAsyncExecution::ThreadPool, [System, K]() { return GeneratePatch(System, K); });
        PendingMeshes.Add(MoveTemp(Job));
    }
}
void AVoyagerWorld::ShowBranch(uint64 K, bool bParentVisible)
{
    FVoyagerTerrainChunk* Chunk = Chunks.Find(K);
    if (!Chunk || !Chunk->Terrain) return;
    bool bChildrenReady = PatchLevel(K) < MaxPatchLevel;
    for (int32 I = 0; I < 4 && bChildrenReady; ++I)
        bChildrenReady = DesiredNodes.Contains(Child(K, I)) && Chunks.Contains(Child(K, I));
    const bool bShow = bParentVisible && !bChildrenReady;
    if (bShow != Chunk->bVisible)
    {
        Chunk->bVisible = bShow;
        Chunk->Terrain->SetVisibility(bShow);
        Chunk->Terrain->SetCollisionEnabled(bShow && Chunk->bCollision ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
    }
    if (PatchLevel(K) < MaxPatchLevel)
        for (int32 I = 0; I < 4; ++I) ShowBranch(Child(K, I), bParentVisible && bChildrenReady);
}
void AVoyagerWorld::UpdatePatchVisibility()
{
    for (int32 P = 0; P < Voyager::PlanetCount; ++P)
        for (int32 F = 0; F < 6; ++F) ShowBranch(PatchKey(P, F, 0, 0, 0), true);
}

void AVoyagerWorld::BuildLighting()
{
    if (GetNetMode() == NM_DedicatedServer) return;
    auto* Sun = Component<UDirectionalLightComponent>(this, TEXT("VoyagerSun"), SceneComponents);
    Sun->SetMobility(EComponentMobility::Movable);
    Sun->SetRelativeRotation(FRotator(-32.f, -47.f, 0.f));
    Sun->SetLightColor(FLinearColor(1.f, .965f, .91f));
    Sun->SetIntensity(9.f);
    Sun->SetCastShadows(true);
    Sun->SetAtmosphereSunLight(true);
    Sun->SetAtmosphereSunLightIndex(0);
    Sun->SetForwardShadingPriority(1);
    Sun->DynamicShadowDistanceMovableLight = 65000.f;
    Sun->DynamicShadowCascades = 4;
    Sun->bUseRayTracedDistanceFieldShadows = false;
    Sun->bPerPixelAtmosphereTransmittance = true;
    Sun->bCastCloudShadows = true;
    Sun->CloudShadowExtent = 35.f;
    Sun->CloudShadowStrength = .42f;
    Sun->LightSourceAngle = .5357f;
    Sun->ContactShadowLength = .03f;
    Sun->RegisterComponent();
    auto* Fill = Component<UDirectionalLightComponent>(this, TEXT("StellarAmbient"), SceneComponents);
    Fill->SetMobility(EComponentMobility::Movable);
    Fill->SetRelativeRotation(FRotator(-50.f, 138.f, 0.f));
    Fill->SetLightColor(FLinearColor(.37f, .54f, .82f));
    Fill->SetIntensity(.12f);
    Fill->SetCastShadows(false);
    Fill->SetAtmosphereSunLight(false);
    Fill->SetForwardShadingPriority(0);
    Fill->RegisterComponent();
    Atmosphere = Component<USkyAtmosphereComponent>(this, TEXT("PhysicalPlanetAtmosphere"), SceneComponents);
    Atmosphere->SetMobility(EComponentMobility::Movable);
    Atmosphere->TransformMode = ESkyAtmosphereTransformMode::PlanetCenterAtComponentTransform;
    Atmosphere->SetAtmosphereHeight(float(Voyager::AtmosphereHeight / Voyager::CentimetersPerKm));
    Atmosphere->SetRayleighExponentialDistribution(4.8f);
    Atmosphere->SetMieExponentialDistribution(1.2f);
    // These are extinction coefficients per kilometer, not unit multipliers.
    // Earth-like optical depth keeps daylight blue and distant terrain visible.
    Atmosphere->SetRayleighScatteringScale(.0331f);
    Atmosphere->SetMieScatteringScale(.003996f);
    Atmosphere->SetMieAbsorptionScale(.000444f);
    Atmosphere->MultiScatteringFactor = 1.f;
    Atmosphere->TraceSampleCountScale = 2.f;
    Atmosphere->RegisterComponent();
    if(auto* CloudBase=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Cosmos/MI_CosmosWeather.MI_CosmosWeather")))
    {
        Clouds=Component<UVolumetricCloudComponent>(this,TEXT("PlanetaryWeather"),SceneComponents);
        Clouds->SetMobility(EComponentMobility::Movable);
        CloudMaterial=UMaterialInstanceDynamic::Create(CloudBase,this);
        Clouds->SetMaterial(CloudMaterial);
        // The atmosphere floor is 3km beneath nominal terrain, so clouds start 2.2km above it.
        Clouds->SetLayerBottomAltitude(5.2f);Clouds->SetLayerHeight(3.2f);
        Clouds->SetTracingStartMaxDistance(20000.f);Clouds->SetTracingMaxDistance(220.f);
        Clouds->SetViewSampleCountScale(1.5f);Clouds->SetShadowViewSampleCountScale(.6f);
        Clouds->SetShadowTracingDistance(8.f);Clouds->SetReflectionViewSampleCountScale(.2f);
        Clouds->SetSkyLightCloudBottomOcclusion(.25f);
        Clouds->SetVisibleInRealTimeSkyCaptures(true);
        Clouds->SetbUsePerSampleAtmosphericLightTransmittance(true);
        Clouds->RegisterComponent();
        // Register resolves the component's soft material reference; rebind the
        // transient runtime instance afterwards so its lifetime is explicit.
        Clouds->SetMaterial(CloudMaterial);
        UE_LOG(LogTemp,Display,TEXT("VOYAGER CLOUD BIND material=%s expected=%s visible=%d"),
            *GetNameSafe(Clouds->GetMaterial()),*GetNameSafe(CloudMaterial),Clouds->IsVisible());
    }
    Skylight = Component<USkyLightComponent>(this, TEXT("PlanetSkylight"), SceneComponents);
    Skylight->SetMobility(EComponentMobility::Movable);
    Skylight->SetIntensity(1.f);
    Skylight->bRealTimeCapture = true;
    Skylight->RegisterComponent();
    auto* Post = Component<UPostProcessComponent>(this, TEXT("VoyagerExposure"), SceneComponents);
    Post->bUnbound = true;
    Post->Settings.bOverride_AutoExposureMethod = true;
    Post->Settings.AutoExposureMethod = EAutoExposureMethod::AEM_Histogram;
    Post->Settings.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
    Post->Settings.AutoExposureApplyPhysicalCameraExposure = false;
    Post->Settings.bOverride_AutoExposureBias = true;
    Post->Settings.AutoExposureBias = -.15f;
    // A restrained four-stop adaptation range keeps interiors readable without
    // turning the night side into daylight. These are luminance, not EV100 units.
    Post->Settings.bOverride_AutoExposureMinBrightness = true;
    Post->Settings.AutoExposureMinBrightness = .08f;
    Post->Settings.bOverride_AutoExposureMaxBrightness = true;
    Post->Settings.AutoExposureMaxBrightness = 1.25f;
    Post->Settings.bOverride_AutoExposureSpeedUp = true;
    Post->Settings.AutoExposureSpeedUp = 3.f;
    Post->Settings.bOverride_AutoExposureSpeedDown = true;
    Post->Settings.AutoExposureSpeedDown = 1.f;
    Post->Settings.bOverride_BloomIntensity = true;
    Post->Settings.BloomIntensity = .12f;
    Post->Settings.bOverride_VignetteIntensity = true;
    Post->Settings.VignetteIntensity = .16f;
    Post->Settings.bOverride_MotionBlurAmount = true;
    Post->Settings.MotionBlurAmount = 0.f;
    Post->Settings.bOverride_AmbientOcclusionIntensity = true;
    Post->Settings.AmbientOcclusionIntensity = .65f;
    Post->Settings.bOverride_AmbientOcclusionRadius = true;
    Post->Settings.AmbientOcclusionRadius = 120.f;
    Post->Settings.bOverride_AmbientOcclusionQuality = true;
    Post->Settings.AmbientOcclusionQuality = 75.f;
    Post->Settings.bOverride_ReflectionMethod = true;
    Post->Settings.ReflectionMethod = EReflectionMethod::ScreenSpace;
    Post->Settings.bOverride_ScreenSpaceReflectionQuality = true;
    Post->Settings.ScreenSpaceReflectionQuality = 65.f;
    Post->RegisterComponent();
}
void AVoyagerWorld::UpdateAtmosphere()
{
    if (!Atmosphere || Views.IsEmpty()) return;
    FVector Eye = Views[0];
    for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
        if (const APlayerController* PC = It->Get())
            if (PC->IsLocalController()) { FRotator Rotation; PC->GetPlayerViewPoint(Eye, Rotation); break; }
    const int32 Nearest = Voyager::NearestPlanet(BuiltSystem, Eye);
    // A capture at the universe origin samples black space on remote planets.
    // Follow the local camera in bounded increments; real-time capture refreshes it.
    if(Skylight&&FVector::DistSquared(Skylight->GetComponentLocation(),Eye)>FMath::Square(5000.0))
        Skylight->SetWorldLocation(Eye);
    const double Altitude = Voyager::SurfaceAltitude(BuiltSystem, Nearest, Eye);
    const FVector Up = Voyager::SurfaceNormal(BuiltSystem, Nearest, Eye);
    const double Daylight = FVector::DotProduct(Up, -FRotator(-32.f, -47.f, 0.f).Vector());
    for (UInstancedStaticMeshComponent* Stars : StarFields)
    {
        Stars->SetWorldLocation(Eye);
        const float EscapeFade=float(FMath::SmoothStep(1600000.0,5000000.0,Altitude));
        const float NightFade=float(1.0-FMath::SmoothStep(-.2,.05,Daylight));
        const float Visibility=FMath::Max(EscapeFade,NightFade);
        Stars->SetVisibility(Visibility>.001f);
        if(auto* Material=Cast<UMaterialInstanceDynamic>(Stars->GetMaterial(0)))Material->SetScalarParameterValue(TEXT("Glow"),Visibility*2.f);
    }
    if (Nearest == AtmospherePlanet) return;
    AtmospherePlanet = Nearest;
    Atmosphere->SetWorldLocation(Voyager::PlanetCenter(BuiltSystem, Nearest));
    // The terrain includes valleys below nominal radius; the atmosphere ground must not clip them.
    Atmosphere->SetBottomRadius(float((Voyager::PlanetRadius(BuiltSystem, Nearest) - 300000.0) / Voyager::CentimetersPerKm));
    Atmosphere->SetAtmosphereHeight(float((Voyager::AtmosphereHeight + 300000.0) / Voyager::CentimetersPerKm));
    Atmosphere->SetGroundAlbedo(FColor(90, 95, 85));
    Atmosphere->SetSkyLuminanceFactor(FLinearColor::White);
    // Rayleigh coefficients preserve blue daylight and reddened grazing paths;
    // humidity/dust vary Mie scattering without tinting the whole scene cyan.
    const int32 Climate=Voyager::Biome(BuiltSystem,Nearest);
    static const float Rayleigh[]={.028f,.023f,.027f,.020f,.029f};
    static const float Aerosol[]={.0042f,.008f,.0027f,.011f,.005f};
    static const float MieHeight[]={1.4f,2.0f,1.05f,1.8f,1.55f};
    Atmosphere->SetRayleighScattering(FLinearColor(.1753f,.4096f,1.f));
    // The optical ground is below valleys; compensate that 3km offset so the
    // nominal terrain still receives the intended sea-level density profile.
    const float RayleighBase=Rayleigh[Climate]*FMath::Exp(3.f/4.8f);
    const float AerosolBase=Aerosol[Climate]*FMath::Exp(3.f/MieHeight[Climate]);
    Atmosphere->SetRayleighScatteringScale(RayleighBase);
    Atmosphere->SetMieScatteringScale(AerosolBase);
    Atmosphere->SetMieAbsorptionScale(AerosolBase*.11f);
    Atmosphere->SetMieAnisotropy(.78f);
    Atmosphere->SetMieExponentialDistribution(MieHeight[Climate]);
    Atmosphere->SetOtherAbsorptionScale(.001881f);
    if(Clouds&&CloudMaterial)
    {
        const FVector C=Voyager::PlanetCenter(BuiltSystem,Nearest);
        const uint32 Seed=uint32(Voyager::PlanetSeed(BuiltSystem,Nearest));
        Clouds->SetWorldLocation(C);
        Clouds->SetPlanetRadius(float(Voyager::PlanetRadius(BuiltSystem,Nearest)/Voyager::CentimetersPerKm));
        Clouds->SetGroundAlbedo(Voyager::BiomeColor(Voyager::Biome(BuiltSystem,Nearest)).ToFColor(true));
        // Native cloud shader uses spherical sample altitude and a weather texture
        // field. These instance controls vary each world's coverage and erosion.
        static const float Coverage[]={.02f,-.23f,-.08f,-.30f,-.04f};
        CloudMaterial->SetScalarParameterValue(TEXT("Cloud_GlobalCoverage"),Coverage[Voyager::Biome(BuiltSystem,Nearest)]);
        CloudMaterial->SetScalarParameterValue(TEXT("Layout_CloudGlobalScale"),192.f+float(Seed%65));
        CloudMaterial->SetVectorParameterValue(TEXT("Cloud_AlbedoColor"),FLinearColor(.98f,.985f,1.f,.5f));
    }
    for (int32 P = 0; P < AtmosphereShells.Num(); ++P)
        if (AtmosphereShells[P]) AtmosphereShells[P]->SetVisibility(P != Nearest);
    for(int32 P=0;P<CloudShells.Num();++P)
        if(CloudShells[P])CloudShells[P]->SetVisibility(P!=Nearest);
}
void AVoyagerWorld::BuildBackdrop()
{
    if (GetNetMode() == NM_DedicatedServer) return;
    FRandomStream Random{int32(Voyager::Hash(uint32(BuiltSystem)))};
    auto* Stars = Instances(this, SceneComponents, TEXT("DeepSpaceStars"), TEXT("Sphere"), FLinearColor(.63f, .78f, 1.f), FVector::ZeroVector, true, true);
    auto* Warm = Instances(this, SceneComponents, TEXT("WarmStars"), TEXT("Sphere"), FLinearColor(1.f, .76f, .48f), FVector::ZeroVector, true, true);
    StarFields.Add(Stars); StarFields.Add(Warm);
    for (int32 I = 0; I < 1200; ++I)
    {
        const FVector Direction = Random.VRand();
        const float Size = Random.FRandRange(90000.f, 240000.f);
        Instance(I % 7 ? Stars : Warm, Direction * 2.e10, FVector(Size));
    }
    // SkyAtmosphere draws the physical sun disk; an extra emissive sphere would
    // double it and ignore atmospheric extinction on approach to the horizon.
    UMaterialInterface* RimBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Cosmos/M_CosmosLimb.M_CosmosLimb"));
    AtmosphereShells.SetNum(Voyager::PlanetCount);
    CloudShells.SetNum(Voyager::PlanetCount);
    UMaterialInterface* CloudBase=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Cosmos/M_CosmosCloudDistant.M_CosmosCloudDistant"));
    UMaterialInterface* OceanBase=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Cosmos/M_CosmosOcean.M_CosmosOcean"));
    if (!RimBase) return;
    for (int32 P = 0; P < Voyager::PlanetCount; ++P)
    {
        auto* Shell = Component<UProceduralMeshComponent>(this, FString::Printf(TEXT("AtmosphereShell%d"), P), SceneComponents);
        Shell->SetRelativeLocation(Voyager::PlanetCenter(BuiltSystem, P));
        Shell->SetMobility(EComponentMobility::Static);
        Shell->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Shell->SetCastShadow(false);
        UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(RimBase, this);
        Material->SetVectorParameterValue(TEXT("Tint"), FLinearColor(.17f,.39f,.86f));
        Material->SetVectorParameterValue(TEXT("SunDirection"),FLinearColor(-FRotator(-32.f,-47.f,0.f).Vector()));
        Shell->SetMaterial(0, Material);
        Shell->RegisterComponent();
        TArray<FVector> V, N;
        TArray<int32> T;
        TArray<FVector2D> UV;
        TArray<FLinearColor> C;
        TArray<FProcMeshTangent> Tangents;
        constexpr int32 Resolution = 48, Stride = Resolution + 1;
        const double Radius = Voyager::PlanetRadius(BuiltSystem, P) + Voyager::AtmosphereHeight * .16;
        for (int32 F = 0; F < 6; ++F)
        {
            const int32 Base = V.Num();
            for (int32 Y = 0; Y <= Resolution; ++Y)
                for (int32 X = 0; X <= Resolution; ++X)
                {
                    const FVector Up = CubeDirection(F, -1.0 + 2.0 * X / Resolution, -1.0 + 2.0 * Y / Resolution);
                    V.Add(Up * Radius); N.Add(Up); UV.Add(FVector2D(double(X) / Resolution, double(Y) / Resolution));
                }
            for (int32 Y = 0; Y < Resolution; ++Y)
                for (int32 X = 0; X < Resolution; ++X)
                {
                    const int32 A = Base + Y * Stride + X, B = A + 1, E = A + Stride, D = E + 1;
                    T.Append({A, E, B, B, E, D});
                }
        }
        Shell->CreateMeshSection_LinearColor(0, V, T, N, UV, C, Tangents, false);
        AtmosphereShells[P] = Shell;
        if(CloudBase)
        {
            auto* Veil=Component<UProceduralMeshComponent>(this,FString::Printf(TEXT("DistantClouds%d"),P),SceneComponents);
            const FVector Center=Voyager::PlanetCenter(BuiltSystem,P);
            const uint32 Seed=uint32(Voyager::PlanetSeed(BuiltSystem,P));
            Veil->SetRelativeLocation(Center);Veil->SetMobility(EComponentMobility::Static);
            Veil->SetCollisionEnabled(ECollisionEnabled::NoCollision);Veil->SetCastShadow(false);
            auto* CloudMID=UMaterialInstanceDynamic::Create(CloudBase,this);
            CloudMID->SetVectorParameterValue(TEXT("PlanetCenter"),FLinearColor(float(Center.X),float(Center.Y),float(Center.Z),0));
            CloudMID->SetVectorParameterValue(TEXT("SeedOffset"),FLinearColor(float(Seed%97)*.117f,float(Seed%73)*.131f,float(Seed%53)*.109f,0));
            static const float Coverage[]={.39f,.56f,.43f,.60f,.41f};
            CloudMID->SetScalarParameterValue(TEXT("CoverageStart"),Coverage[Voyager::Biome(BuiltSystem,P)]);
            Veil->SetMaterial(0,CloudMID);Veil->RegisterComponent();
            for(FVector& Vertex:V)Vertex*=((Voyager::PlanetRadius(BuiltSystem,P)+500000.0)/Radius);
            Veil->CreateMeshSection_LinearColor(0,V,T,N,UV,C,Tangents,false);
            CloudShells[P]=Veil;
        }
        // A spherical sea intersects the very same terrain field seen on foot.
        // It sits 450m below nominal terrain, below the settlement clearings.
        // Frozen worlds retain ice terrain; only temperate biomes have open water.
        const int32 Climate=Voyager::Biome(BuiltSystem,P);
        if(OceanBase&&(Climate==0||Climate==4))
        {
            auto* Ocean=Component<UProceduralMeshComponent>(this,FString::Printf(TEXT("PlanetaryOcean%d"),P),SceneComponents);
            Ocean->SetRelativeLocation(Voyager::PlanetCenter(BuiltSystem,P));Ocean->SetMobility(EComponentMobility::Static);
            Ocean->SetCollisionEnabled(ECollisionEnabled::NoCollision);Ocean->SetCastShadow(false);
            auto* OceanMaterial=UMaterialInstanceDynamic::Create(OceanBase,this);
            OceanMaterial->SetVectorParameterValue(TEXT("Tint"),Climate==0?FLinearColor(.006f,.036f,.060f):FLinearColor(.009f,.029f,.049f));
            Ocean->SetMaterial(0,OceanMaterial);Ocean->RegisterComponent();
            TArray<FVector> SeaVertices,SeaNormals;TArray<FVector2D> SeaUV;TArray<int32> SeaTriangles;
            constexpr int32 SeaResolution=160,SeaStride=SeaResolution+1;
            const double SeaRadius=Voyager::PlanetRadius(BuiltSystem,P)-45000.0;
            for(int32 Face=0;Face<6;++Face)
            {
                const int32 Base=SeaVertices.Num();
                for(int32 Y=0;Y<=SeaResolution;++Y)for(int32 X=0;X<=SeaResolution;++X)
                {
                    const FVector Up=CubeDirection(Face,-1.0+2.0*X/SeaResolution,-1.0+2.0*Y/SeaResolution);
                    SeaVertices.Add(Up*SeaRadius);SeaNormals.Add(Up);SeaUV.Add(FVector2D(double(X)/SeaResolution,double(Y)/SeaResolution));
                }
                for(int32 Y=0;Y<SeaResolution;++Y)for(int32 X=0;X<SeaResolution;++X)
                {
                    const int32 A=Base+Y*SeaStride+X,B=A+1,E=A+SeaStride,D=E+1;SeaTriangles.Append({A,E,B,B,E,D});
                }
            }
            Ocean->CreateMeshSection_LinearColor(0,SeaVertices,SeaTriangles,SeaNormals,SeaUV,C,Tangents,false);
        }
    }
}

void AVoyagerWorld::StreamDetails(bool bSynchronous)
{
    TSet<uint64> Wanted;
    for (FVector View : Views)
    {
        const int32 P = Voyager::NearestPlanet(BuiltSystem, View);
        if (Voyager::SurfaceAltitude(BuiltSystem, P, View) > 35000.0) continue;
        const FVector Up = Voyager::SurfaceNormal(BuiltSystem, P, View);
        const int32 Face = DominantFace(Up), Count = 1 << DetailLevel;
        double U = 0, V = 0;
        FaceUV(Up, Face, U, V);
        const int32 CX = FMath::Clamp(FMath::FloorToInt((U + 1.0) * .5 * Count), 0, Count - 1);
        const int32 CY = FMath::Clamp(FMath::FloorToInt((V + 1.0) * .5 * Count), 0, Count - 1);
        for (int32 Y = -4; Y <= 4; ++Y)
            for (int32 X = -4; X <= 4; ++X)
            {
                // Directions outside one cube face naturally remap onto its neighbour.
                const FVector Direction = CubeDirection(Face, -1.0 + (CX + X + .5) * 2.0 / Count,
                    -1.0 + (CY + Y + .5) * 2.0 / Count);
                const int32 F = DominantFace(Direction);
                double DU = 0, DV = 0;
                FaceUV(Direction, F, DU, DV);
                const int32 DX = FMath::Clamp(FMath::FloorToInt((DU + 1.0) * .5 * Count), 0, Count - 1);
                const int32 DY = FMath::Clamp(FMath::FloorToInt((DV + 1.0) * .5 * Count), 0, Count - 1);
                Wanted.Add(PatchKey(P, F, DetailLevel, DX, DY));
            }
    }
    TArray<uint64> Order = Wanted.Array();
    TMap<uint64, double> Distances;
    for (uint64 K : Order) Distances.Add(K, PatchDistance(BuiltSystem, K, Views));
    Order.Sort([&Distances](uint64 A, uint64 B)
    {
        const double DA = Distances.FindChecked(A), DB = Distances.FindChecked(B);
        return DA == DB ? A < B : DA < DB;
    });
    if (Order.Num() > MaxDetailCells) Order.SetNum(MaxDetailCells);
    Wanted.Reset();
    for (uint64 K : Order) Wanted.Add(K);
    TArray<uint64> Old;
    DetailCells.GetKeys(Old);
    for (uint64 K : Old) if (!Wanted.Contains(K)) RemoveDetailCell(K);
    int32 Added = 0;
    for (uint64 K : Order)
        if (!DetailCells.Contains(K))
        {
            BuildDetailCell(K);
            if (!bSynchronous && ++Added >= 4) break;
        }
}
void AVoyagerWorld::RemoveDetailCell(uint64 K)
{
    FVoyagerTerrainChunk* Cell = DetailCells.Find(K);
    if (!Cell) return;
    if (HasAuthority()) for (AVoyagerResource* Resource : Cell->Resources) if (IsValid(Resource)) Resource->Destroy();
    Dispose(this, Cell->Components);
    DetailCells.Remove(K);
}
void AVoyagerWorld::BuildDetailCell(uint64 K)
{
    if (DetailCells.Contains(K) || DetailCells.Num() >= MaxDetailCells) return;
    FVoyagerTerrainChunk& Cell = DetailCells.Add(K);
    const int32 P = PatchPlanet(K), F = PatchFace(K), B = Voyager::Biome(BuiltSystem, P);
    const double Count = double(1 << DetailLevel), Span = 2.0 / Count;
    const double U0 = -1.0 + PatchX(K) * Span, V0 = -1.0 + PatchY(K) * Span;
    const double Radius = Voyager::PlanetRadius(BuiltSystem, P);
    Cell.Origin = Voyager::SurfacePoint(BuiltSystem, P, PatchDirection(K));
    const uint32 Seed = Voyager::Hash(uint32(Voyager::PlanetSeed(BuiltSystem, P)) ^ uint32(K) ^ uint32(K >> 32));
    // Natural decoration is streamed by AVoyagerVegetation; these cells own resources.
    if (!HasAuthority()) return;
    int32 ResourceCount = 0;
    for (const auto& Pair : DetailCells)
        for (AVoyagerResource* Resource : Pair.Value.Resources) if (IsValid(Resource)) ++ResourceCount;
    FRandomStream ResourceRandom(int32(Seed ^ 0x31857a91u));
    for (int32 I = 0; I < 2 && ResourceCount < MaxResources; ++I)
    {
        const uint64 ResourceID = (K << 2) | uint64(I + 1);
        if (HarvestedResources.Contains(ResourceID)) continue;
        FVector Up = CubeDirection(F, U0 + Span * ResourceRandom.FRandRange(.15f, .85f), V0 + Span * ResourceRandom.FRandRange(.15f, .85f));
        if (F == 4 && PatchX(K) == (1 << (DetailLevel - 1)) - 1 && PatchY(K) == (1 << (DetailLevel - 1)))
            Up = FVector(-700.0 - I * 210.0, 500.0 + I * 230.0, Radius).GetSafeNormal();
        else if ((Up - FVector::UpVector).Size() * Radius < 900.0) continue;
        if(AVoyagerSettlement::IsWithinSite(BuiltSystem,P,Up,600.0))continue;
        const FVector Location = Voyager::SurfacePoint(BuiltSystem, P, Up, 120.0);
        const FTransform Transform(Voyager::TangentRotation(Up), Location);
        AVoyagerResource* Resource = GetWorld()->SpawnActorDeferred<AVoyagerResource>(AVoyagerResource::StaticClass(),
            Transform, this, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
        if (!Resource) continue;
        Resource->Kind = (B + I) % 3;
        Resource->Value = 12 + ResourceRandom.RandRange(0, 12);
        Resource->ResourceKey = ResourceID;
        UGameplayStatics::FinishSpawningActor(Resource, Transform);
        Cell.Resources.Add(Resource);
        ++ResourceCount;
    }
}
void AVoyagerWorld::MarkResourceHarvested(uint64 Key)
{
    if (!HasAuthority() || HarvestedResources.Contains(Key)) return;
    // Remember recent extraction without placing a hard resource-generation limit on exploration.
    if (HarvestedOrder.Num() >= 4096)
    {
        HarvestedResources.Remove(HarvestedOrder[0]);
        HarvestedOrder.RemoveAt(0);
    }
    HarvestedResources.Add(Key);
    HarvestedOrder.Add(Key);
}
void AVoyagerWorld::BuildLandmarks()
{
    for (int32 P = 0; P < Voyager::PlanetCount; ++P)
    {
        const double Radius = Voyager::PlanetRadius(BuiltSystem, P);
        const auto GroundAt = [this, P, Radius](double X, double Y, double H)
        {
            return Voyager::SurfacePoint(BuiltSystem, P, FVector(X, Y, Radius).GetSafeNormal(), H);
        };
        const FLinearColor Metal(.12f, .19f, .23f), Light(.2f, .85f, 1.f), Stone(.31f, .33f, .3f);
        const FString Prefix = FString::Printf(TEXT("Planet%d_"), P);
        Part(this, SceneComponents, Prefix + TEXT("LandingPad"), TEXT("Cylinder"), GroundAt(400, 0, -16),
            FVector(12.f, 12.f, .3f), Metal);
        auto* Markers = Instances(this, SceneComponents, Prefix + TEXT("LandingLights"), TEXT("Cube"), Light,
            Voyager::SurfacePoint(BuiltSystem, P, FVector::UpVector), true);
        for (int32 I = 0; I < 12; ++I)
        {
            const double A = I * UE_TWO_PI / 12.0;
            Instance(Markers, GroundAt(400 + FMath::Cos(A) * 570, FMath::Sin(A) * 570, 4) - Markers->GetComponentLocation(),
                FVector(.8f, .15f, .055f), FRotator(0, float(FMath::RadiansToDegrees(A) + 90), 0));
        }
        const FVector Ruin = GroundAt(2100, 900, 0);
        const FVector Up = Voyager::SurfaceNormal(BuiltSystem, P, Ruin);
        const FQuat Frame = Voyager::TangentRotation(Up).Quaternion();
        Part(this, SceneComponents, Prefix + TEXT("ArchiveBase"), TEXT("Cylinder"), Ruin + Up * 8,
            FVector(9.f, 9.f, .16f), Stone, false, Frame.Rotator(), true);
        for (int32 I = 0; I < 4; ++I)
        {
            const double A = I * UE_HALF_PI;
            const FVector Offset = Frame.RotateVector(FVector(FMath::Cos(A) * 280, FMath::Sin(A) * 280, 0));
            Part(this, SceneComponents, Prefix + FString::Printf(TEXT("ArchivePillar%d"), I), TEXT("Cube"),
                Ruin + Offset + Up * 220, FVector(.95f, .95f, 4.4f), Stone, false, Frame.Rotator(), true);
            Part(this, SceneComponents, Prefix + FString::Printf(TEXT("ArchiveRune%d"), I), TEXT("Cube"),
                Ruin + Offset + Up * 235 - Frame.GetAxisY() * 49, FVector(.23f, .05f, 1.9f), Light, true, Frame.Rotator());
        }
        Part(this, SceneComponents, Prefix + TEXT("ArchiveCore"), TEXT("Sphere"), Ruin + Up * 205, FVector(1.45f), Light, true);
        Part(this, SceneComponents, Prefix + TEXT("ArchiveCanopy"), TEXT("Cube"), Ruin + Up * 445,
            FVector(6.9f, 6.9f, .4f), Stone, false, Frame.Rotator(), true);
    }
}

AVoyagerResource::AVoyagerResource()
{
    bReplicates = true;
    SetReplicateMovement(false);
    SetNetCullDistanceSquared(FMath::Square(35000.f));
    Crystal = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MineralCrystal"));
    SetRootComponent(Crystal);
    Crystal->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cone.Cone")));
    Crystal->SetRelativeScale3D(FVector(1.35f, 1.35f, 2.4f));
    Crystal->SetCollisionProfileName(TEXT("BlockAll"));
    Crystal->SetCollisionObjectType(ECC_WorldStatic);
    Crystal->SetCanEverAffectNavigation(false);
    for (int32 I = 0; I < 3; ++I)
    {
        auto* Shard = CreateDefaultSubobject<UStaticMeshComponent>(*FString::Printf(TEXT("MineralShard%d"), I));
        Shard->SetupAttachment(Crystal);
        Shard->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cone.Cone")));
        Shard->SetRelativeLocation(FVector(FMath::Cos(I * UE_TWO_PI / 3.f) * 31.f, FMath::Sin(I * UE_TWO_PI / 3.f) * 31.f, -19.f));
        Shard->SetRelativeScale3D(FVector(.62f, .62f, .64f));
        Shard->SetRelativeRotation(FRotator(12.f, I * 120.f, 0));
        Shard->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Shards.Add(Shard);
    }
}
void AVoyagerResource::BeginPlay() { Super::BeginPlay(); OnRep_State(); }
void AVoyagerResource::OnRep_State()
{
    static const FLinearColor Colors[] = {FLinearColor(.17f, .92f, 1.f), FLinearColor(1.f, .46f, .1f), FLinearColor(.73f, .28f, 1.f)};
    UMaterialInstanceDynamic* Material = RiftVisual::Material(this, Colors[FMath::Clamp(Kind, 0, 2)], true);
    Crystal->SetMaterial(0, Material);
    for (UStaticMeshComponent* Shard : Shards) Shard->SetMaterial(0, Material);
    SetActorHiddenInGame(bHarvested);
    SetActorEnableCollision(!bHarvested);
}
FString AVoyagerResource::ResourceName() const
{
    static const TCHAR* Names[] = {TEXT("AZURITE DEPOSIT"), TEXT("SOLARITE DEPOSIT"), TEXT("VOID QUARTZ")};
    return Names[FMath::Clamp(Kind, 0, 2)];
}
float AVoyagerResource::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
    AController* EventInstigator, AActor* DamageCauser)
{
    if (!HasAuthority() || bHarvested || DamageAmount <= 0.f) return 0.f;
    Health -= DamageAmount;
    if (Health <= 0.f)
    {
        bHarvested = true;
        if (AVoyagerWorld* Builder = Cast<AVoyagerWorld>(GetOwner())) Builder->MarkResourceHarvested(ResourceKey);
        if (EventInstigator)
            if (AVoyagerPlayerState* PlayerState = EventInstigator->GetPlayerState<AVoyagerPlayerState>()) PlayerState->AddMinerals(Value);
        OnRep_State();
        ForceNetUpdate();
        SetLifeSpan(.15f);
    }
    return DamageAmount;
}
void AVoyagerResource::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(AVoyagerResource, Value);
    DOREPLIFETIME(AVoyagerResource, Kind);
    DOREPLIFETIME(AVoyagerResource, bHarvested);
    DOREPLIFETIME(AVoyagerResource, Health);
}
