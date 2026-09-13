#include "VoyagerItemVisuals.h"

#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "KismetProceduralMeshLibrary.h"
#include "Misc/AutomationTest.h"
#endif

namespace VoyagerItemVisuals
{
    namespace
    {
        constexpr int32 MaterialCount = 3;
        struct FSection
        {
            TArray<FVector> Positions, Normals;
            TArray<FVector2D> UV;
            TArray<int32> Triangles;
            TArray<FProcMeshTangent> Tangents;
        };
        struct FModel
        {
            FSection Sections[MaterialCount];
            FLinearColor Colors[MaterialCount];
            float Roughness[MaterialCount] = { .7f, .65f, .5f };
            float Metallic[MaterialCount] = { 0.f, 0.f, 0.f };
        };

        bool IsValid(EVoyagerItem Item)
        {
            return int32(Item) >= 0 && int32(Item) < int32(EVoyagerItem::Count);
        }

        void Triangle(FSection& Section, const FVector& A, const FVector& B, const FVector& C)
        {
            const FVector Normal = FVector::CrossProduct(B - A, C - A).GetSafeNormal();
            if (Normal.IsNearlyZero()) return;
            const int32 Start = Section.Positions.Num();
            Section.Positions.Append({ A, B, C });
            Section.Normals.Append({ Normal, Normal, Normal });
            // Unreal's procedural front faces are clockwise. Keep outward
            // shading normals, but emit the opposite index order (the native
            // UKismetProceduralMeshLibrary::GenerateBoxMesh follows this rule).
            Section.Triangles.Append({ Start, Start + 2, Start + 1 });
            // Consistent local planar UVs also allow future authored material maps.
            const FVector Tangent = (B - A).GetSafeNormal();
            const FVector Bitangent = FVector::CrossProduct(Normal, Tangent);
            for (const FVector& P : { A, B, C })
            {
                Section.UV.Add(FVector2D(FVector::DotProduct(P, Tangent), FVector::DotProduct(P, Bitangent)) / 20.0);
                Section.Tangents.Add(FProcMeshTangent(Tangent, false));
            }
        }

        void Quad(FSection& Section, const FVector& A, const FVector& B, const FVector& C, const FVector& D)
        {
            Triangle(Section, A, B, C);
            Triangle(Section, A, C, D);
        }

        // Eight-sided case with a real chamfer around both faces, not intersecting
        // scaled cubes. All faces are closed and have outward winding.
        void Case(FSection& Section, const FVector& Center, const FVector& Extent, double Bevel)
        {
            const double B = FMath::Clamp(Bevel, .02, FMath::Min3(Extent.X, Extent.Y, Extent.Z) * .8);
            FVector Rings[4][8];
            for (int32 Ring = 0; Ring < 4; ++Ring)
            {
                const bool bFace = Ring == 0 || Ring == 3;
                const double Inset = bFace ? B * .6 : 0.0;
                const double X = Extent.X - Inset, Y = Extent.Y - Inset;
                const double Z = Ring == 0 ? -Extent.Z : Ring == 1 ? -Extent.Z + B : Ring == 2 ? Extent.Z - B : Extent.Z;
                const FVector2D Outline[] = { {X-B,-Y},{X,-Y+B},{X,Y-B},{X-B,Y},
                    {-X+B,Y},{-X,Y-B},{-X,-Y+B},{-X+B,-Y} };
                for (int32 I = 0; I < 8; ++I) Rings[Ring][I] = Center + FVector(Outline[I].X, Outline[I].Y, Z);
            }
            for (int32 I = 0; I < 8; ++I)
            {
                const int32 J = (I + 1) % 8;
                Triangle(Section, Center - FVector(0,0,Extent.Z), Rings[0][J], Rings[0][I]);
                Triangle(Section, Center + FVector(0,0,Extent.Z), Rings[3][I], Rings[3][J]);
                for (int32 Ring = 0; Ring < 3; ++Ring)
                    Quad(Section, Rings[Ring][I], Rings[Ring][J], Rings[Ring+1][J], Rings[Ring+1][I]);
            }
        }

        // Smooth normals keep organic ingredients readable without full skeletal
        // or imported meshes. No duplicated zero-area pole triangles are emitted.
        void Oval(FSection& Section, const FVector& Center, const FVector& Extent, double Yaw = 0.0)
        {
            constexpr int32 Sides = 16, Bands = 8;
            const FQuat Rotation(FVector::UpVector, FMath::DegreesToRadians(Yaw));
            const int32 Start = Section.Positions.Num();
            for (int32 Ring = 0; Ring <= Bands; ++Ring)
            {
                const double V = double(Ring) / Bands, Latitude = UE_DOUBLE_PI * V;
                for (int32 Side = 0; Side <= Sides; ++Side)
                {
                    const double U = double(Side) / Sides, Longitude = UE_DOUBLE_PI * 2.0 * U;
                    const FVector Sphere(FMath::Sin(Latitude) * FMath::Cos(Longitude),
                        FMath::Sin(Latitude) * FMath::Sin(Longitude), FMath::Cos(Latitude));
                    Section.Positions.Add(Center + Rotation.RotateVector(Sphere * Extent));
                    Section.Normals.Add(Rotation.RotateVector((Sphere / Extent).GetSafeNormal()));
                    Section.UV.Add(FVector2D(U, V));
                    Section.Tangents.Add(FProcMeshTangent(Rotation.RotateVector(FVector(-FMath::Sin(Longitude), FMath::Cos(Longitude), 0)), false));
                }
            }
            for (int32 Ring = 0; Ring < Bands; ++Ring)
                for (int32 Side = 0; Side < Sides; ++Side)
                {
                    const int32 A = Start + Ring * (Sides + 1) + Side, B = A + Sides + 1;
                    if (Ring > 0) Section.Triangles.Append({ A, A + 1, B });
                    if (Ring < Bands - 1) Section.Triangles.Append({ A + 1, B + 1, B });
                }
        }

        FModel Generate(EVoyagerItem Item)
        {
            FModel Model;
            FSection& Body = Model.Sections[0];
            FSection& Trim = Model.Sections[1];
            FSection& Detail = Model.Sections[2];
            switch (Item)
            {
            case EVoyagerItem::RawMeat:
                Model.Colors[0] = FLinearColor(.31f,.034f,.028f);
                Model.Colors[1] = FLinearColor(.58f,.36f,.24f);
                Model.Colors[2] = FLinearColor(.43f,.085f,.06f);
                Model.Roughness[0] = .42f; Model.Roughness[1] = .55f;
                // Irregular steak with a pale fat rim and muscle lobes.
                Oval(Trim, FVector(-1,0,-.3), FVector(12.6,8.7,3.1), -8);
                Oval(Body, FVector(-.8,0,.25), FVector(11.8,7.7,3.0), -8);
                Oval(Body, FVector(8.2,1,0), FVector(5,5.4,2.8), 19);
                Oval(Detail, FVector(-3.1,-1,2.8), FVector(6,2.4,.5), 28);
                Oval(Trim, FVector(1.2,1.6,2.9), FVector(.55,5.4,.32), -32);
                break;
            case EVoyagerItem::CookedMeat:
                Model.Colors[0] = FLinearColor(.19f,.078f,.028f);
                Model.Colors[1] = FLinearColor(.32f,.34f,.30f);
                Model.Colors[2] = FLinearColor(.046f,.024f,.015f);
                Model.Roughness[0] = .72f; Model.Metallic[1] = .65f;
                Case(Trim, FVector(0,0,-2.4), FVector(14,10,1), .7);
                Oval(Body, FVector(0,0,.2), FVector(11.8,7.6,3.1), -6);
                for (int32 I = -2; I <= 2; ++I)
                    Oval(Detail, FVector(I*3.5,0,2.8), FVector(.35,5.7,.2), 9);
                break;
            case EVoyagerItem::Hide:
                Model.Colors[0] = FLinearColor(.23f,.125f,.055f);
                Model.Colors[1] = FLinearColor(.39f,.24f,.12f);
                Model.Colors[2] = FLinearColor(.065f,.057f,.039f);
                Model.Roughness[0] = .92f; Model.Roughness[1] = .94f;
                // A folded pelt, with irregular lobes and two fastening straps.
                Oval(Body, FVector(0,0,-.6), FVector(18.5,13,3.5), 3);
                Oval(Trim, FVector(-1,1,1.4), FVector(17.5,12,2.6), -5);
                Oval(Body, FVector(12,7,0), FVector(7.5,6,2.7), 16);
                for (double X : { -8.0, 8.0 })
                {
                    Case(Detail, FVector(X,0,3.5), FVector(1.2,12.1,.6), .3);
                    Case(Detail, FVector(X,0,-3), FVector(1.2,12.1,.6), .3);
                }
                break;
            case EVoyagerItem::Bone:
                Model.Colors[0] = FLinearColor(.62f,.53f,.36f);
                Model.Colors[1] = FLinearColor(.46f,.38f,.26f);
                Model.Colors[2] = Model.Colors[1];
                Model.Roughness[0] = .78f;
                Oval(Body, FVector(0,0,0), FVector(15,2.4,2.2));
                for (double X : { -13.0, 13.0 })
                {
                    Oval(Body, FVector(X,-2,0), FVector(4,3.4,3.4), X);
                    Oval(Trim, FVector(X,2,0), FVector(3.7,3.3,3.2), -X);
                }
                break;
            case EVoyagerItem::Medkit:
                Model.Colors[0] = FLinearColor(.48f,.51f,.46f);
                Model.Colors[1] = FLinearColor(.037f,.048f,.045f);
                Model.Colors[2] = FLinearColor(.027f,.25f,.13f);
                Model.Roughness[0] = .55f; Model.Roughness[1] = .72f;
                Case(Body, FVector(0,0,0), FVector(15,11,4.7), 1.2);
                Case(Trim, FVector(0,0,0), FVector(15.15,11.15,.3), .15);
                Case(Detail, FVector(0,0,4.86), FVector(1.2,4.4,.15), .1);
                Case(Detail, FVector(0,0,4.87), FVector(4.4,1.2,.16), .1);
                for (double X : { -8.0, 8.0 }) Case(Trim, FVector(X,-10.9,1), FVector(1.4,.7,1.8), .35);
                Case(Trim, FVector(0,12.5,0), FVector(4.6,.8,1), .4);
                for (double X : { -4.0, 4.0 }) Case(Trim, FVector(X,11.5,0), FVector(.7,1.1,1), .3);
                break;
            case EVoyagerItem::DemolitionCharge:
                Model.Colors[0] = FLinearColor(.11f,.135f,.12f);
                Model.Colors[1] = FLinearColor(.038f,.043f,.046f);
                Model.Colors[2] = FLinearColor(.62f,.27f,.045f);
                Model.Roughness[0] = .59f; Model.Metallic[0] = .3f;
                Case(Trim, FVector(0,0,-2), FVector(13.5,9,2), .8);
                Case(Body, FVector(0,0,1.3), FVector(12.7,8.1,2.2), .9);
                Case(Trim, FVector(0,0,3.6), FVector(4,3,.3), .25);
                Case(Detail, FVector(-9.6,0,3.6), FVector(1.1,6.8,.25), .15);
                Case(Detail, FVector(9.6,0,3.6), FVector(1.1,6.8,.25), .15);
                // Mechanical arming cover and contact feet, with no blinking state
                // implied by this inert inventory representation.
                Case(Detail, FVector(0,0,4), FVector(2.6,1.6,.4), .3);
                for (double X : { -10.0, 10.0 })
                    for (double Y : { -6.0, 6.0 })
                        Case(Trim, FVector(X,Y,-4.3), FVector(2.2,1.5,.7), .4);
                break;
            case EVoyagerItem::EnergyCell:
                Model.Colors[0] = FLinearColor(.22f,.27f,.28f);
                Model.Colors[1] = FLinearColor(.039f,.052f,.057f);
                Model.Colors[2] = FLinearColor(.47f,.31f,.1f);
                Model.Roughness[0] = .42f; Model.Metallic[0] = .7f; Model.Metallic[2] = .8f;
                // Compact cartridge: recessed ribs and two exposed contact pads.
                Case(Body, FVector(0,0,0), FVector(8,4.6,2.7), .8);
                Case(Trim, FVector(-7.8,0,0), FVector(.7,4.5,2.6), .4);
                for (int32 I = -2; I <= 2; ++I)
                    Case(Trim, FVector(I*2.3,0,2.6), FVector(.3,3.4,.25), .15);
                for (double Y : { -2.0, 2.0 })
                    Case(Detail, FVector(8.2,Y,0), FVector(.6,1,1.4), .35);
                break;
            default: break;
            }
            return Model;
        }

        bool Check(const FModel& Model, const FVector& Bounds, FString& Failure)
        {
            int32 TriangleCount = 0;
            for (const FSection& Section : Model.Sections)
            {
                const int32 Count = Section.Positions.Num();
                if (Section.Normals.Num() != Count || Section.UV.Num() != Count || Section.Tangents.Num() != Count || Section.Triangles.Num() % 3 != 0)
                { Failure = TEXT("Mismatched attributes or incomplete triangle"); return false; }
                for (int32 I = 0; I < Count; ++I)
                {
                    const FVector& P = Section.Positions[I];
                    const FVector& N = Section.Normals[I];
                    if (P.ContainsNaN() || N.ContainsNaN() || Section.UV[I].ContainsNaN() ||
                        Section.Tangents[I].TangentX.ContainsNaN() || !FMath::IsNearlyEqual(N.SizeSquared(), 1.0, .001) ||
                        FMath::Abs(P.X) > Bounds.X + .001 || FMath::Abs(P.Y) > Bounds.Y + .001 || FMath::Abs(P.Z) > Bounds.Z + .001)
                    { Failure = TEXT("Non-finite attribute, invalid normal or geometry outside proxy"); return false; }
                }
                for (int32 I = 0; I < Section.Triangles.Num(); I += 3)
                {
                    const int32 A = Section.Triangles[I], B = Section.Triangles[I+1], C = Section.Triangles[I+2];
                    if (!Section.Positions.IsValidIndex(A) || !Section.Positions.IsValidIndex(B) || !Section.Positions.IsValidIndex(C))
                    { Failure = TEXT("Index outside vertex array"); return false; }
                    const FVector Area = FVector::CrossProduct(Section.Positions[B] - Section.Positions[A], Section.Positions[C] - Section.Positions[A]);
                    // The geometric cross product is opposite the outward
                    // shading normal for Unreal's clockwise front-face winding.
                    if (Area.SizeSquared() < 1.e-10 || FVector::DotProduct(Area, Section.Normals[A] + Section.Normals[B] + Section.Normals[C]) >= 0)
                    { Failure = TEXT("Degenerate triangle or reversed winding"); return false; }
                    ++TriangleCount;
                }
            }
            if (TriangleCount == 0 || TriangleCount > 3000)
            { Failure = TEXT("Empty item or close-range geometry budget exceeded"); return false; }
            Failure.Reset(); return true;
        }
    }

    FVector HalfExtent(EVoyagerItem Item)
    {
        switch (Item)
        {
        case EVoyagerItem::RawMeat: return FVector(14,10,4);
        case EVoyagerItem::CookedMeat: return FVector(14,10,3.5);
        case EVoyagerItem::Hide: return FVector(20,15,4.5);
        case EVoyagerItem::Bone: return FVector(18,6,3.5);
        case EVoyagerItem::Medkit: return FVector(15.5,13.5,5.1);
        case EVoyagerItem::DemolitionCharge: return FVector(13.5,9,5);
        case EVoyagerItem::EnergyCell: return FVector(9,4.6,2.9);
        default: return FVector::ZeroVector;
        }
    }

    bool ValidateGeometry(EVoyagerItem Item, FString& Failure)
    {
        if (!IsValid(Item)) { Failure = TEXT("Unknown item"); return false; }
        return Check(Generate(Item), HalfExtent(Item), Failure);
    }

#if WITH_DEV_AUTOMATION_TESTS
    IMPLEMENT_SIMPLE_AUTOMATION_TEST(FItemNativeWindingTest,
        "Riftbound.Voyager.Items.NativeFrontFaceWinding",
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

    bool FItemNativeWindingTest::RunTest(const FString& Parameters)
    {
        // Ground truth comes from the engine's own box generator. This catches
        // a generator and validator that agree with each other but both emit
        // backfaces, which happened in the first rendered medkit capture.
        FModel Reference;
        FSection& Box=Reference.Sections[0];
        UKismetProceduralMeshLibrary::GenerateBoxMesh(FVector(1),Box.Positions,Box.Triangles,
            Box.Normals,Box.UV,Box.Tangents);
        FString Failure;
        TestTrue(TEXT("Native Unreal box winding passes item validation"),Check(Reference,FVector(1),Failure));
        Swap(Box.Triangles[1],Box.Triangles[2]);
        TestFalse(TEXT("A reversed native face fails item validation"),Check(Reference,FVector(1),Failure));
        for(int32 Item=0;Item<int32(EVoyagerItem::Count);++Item)
            TestTrue(VoyagerItems::Name(EVoyagerItem(Item)),ValidateGeometry(EVoyagerItem(Item),Failure));
        return true;
    }
#endif

    UProceduralMeshComponent* Build(AActor* Owner, USceneComponent* Parent, EVoyagerItem Item)
    {
        if (!Owner || !Parent || !IsValid(Item) || Owner->GetNetMode() == NM_DedicatedServer) return nullptr;
        FModel Model = Generate(Item);
        FString Failure;
        if (!Check(Model, HalfExtent(Item), Failure))
        {
            UE_LOG(LogTemp, Error, TEXT("VOYAGER ITEM GEOMETRY item=%d failure=%s"), int32(Item), *Failure);
            return nullptr;
        }
        UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Ships/Materials/M_KestrelInterior.M_KestrelInterior"));
        if (!Base) Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_Surface.M_Surface"));
        if (!Base) Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
        auto* Mesh = NewObject<UProceduralMeshComponent>(Owner);
        Mesh->SetupAttachment(Parent);
        Mesh->SetMobility(EComponentMobility::Movable);
        Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Mesh->SetGenerateOverlapEvents(false);
        Mesh->SetCanEverAffectNavigation(false);
        Mesh->SetCastShadow(true);
        Mesh->SetComponentTickEnabled(false);
        Owner->AddInstanceComponent(Mesh);
        const TArray<FLinearColor> Colors;
        for (int32 I = 0; I < MaterialCount; ++I)
        {
            const FSection& Section = Model.Sections[I];
            if (Section.Triangles.IsEmpty()) continue;
            Mesh->CreateMeshSection_LinearColor(I, Section.Positions, Section.Triangles, Section.Normals,
                Section.UV, Colors, Section.Tangents, false);
            if (auto* Material = UMaterialInstanceDynamic::Create(Base, Mesh))
            {
                Material->SetVectorParameterValue(TEXT("Tint"), Model.Colors[I]);
                Material->SetVectorParameterValue(TEXT("Color"), Model.Colors[I]);
                Material->SetScalarParameterValue(TEXT("Roughness"), Model.Roughness[I]);
                Material->SetScalarParameterValue(TEXT("Metallic"), Model.Metallic[I]);
                Mesh->SetMaterial(I, Material);
            }
        }
        Mesh->RegisterComponent();
        return Mesh;
    }
}
