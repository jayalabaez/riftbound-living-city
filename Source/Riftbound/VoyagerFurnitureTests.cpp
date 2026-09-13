#include "VoyagerFurnitureValidation.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoyagerFurnitureRegression,
    "Riftbound.Voyager.Furniture.FacingSupportAndRadialFrames",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVoyagerFurnitureRegression::RunTest(const FString& Parameters)
{
    using namespace VoyagerFurniture;
    const FVector Table(530,-270,0);
    // Both cafe sides reproduce the original inverted-backrest bug. Additional
    // angles ensure the contract does not secretly assume world +Y or north pole.
    for (const FVector Offset:{FVector(0,-110,0),FVector(0,110,0),FVector(-110,0,0),
        FVector(110,0,0),FVector(77,-77,0),FVector(-77,77,0)})
    {
        const FVector User=Table+Offset;
        for (bool bDesk:{false,true})
        {
            FAssembly Assembly=bDesk?Workstation(Table,User,280):Chair(User,Table);
            FString Reason;
            const bool bValid=Validate(Assembly,Reason);
            TestTrue(*FString::Printf(TEXT("Assembly faces its table and supports items: %s"),*Reason),bValid);
            for (const FQuat Radial:{FQuat::Identity,FRotator(0,137,0).Quaternion(),
                FRotator(82,-34,71).Quaternion(),FRotator(-89,168,-23).Quaternion()})
            {
                const FVector Origin(5.e8,-8.e8,2.e8);
                const FVector WorldTarget=Origin+Radial.RotateVector(Table);
                const FVector Up=Radial.GetAxisZ();
                const FPart* Seat=nullptr;
                const FPart* Back=nullptr;
                for (const FPart& Part:Assembly.Parts)
                {
                    if (Part.Kind==EPart::Seat) Seat=&Part;
                    if (Part.Kind==EPart::Backrest) Back=&Part;
                    const FQuat WorldRotation=Radial*Part.Rotation;
                    TestTrue(TEXT("Furniture shares radial floor normal"),FVector::DotProduct(WorldRotation.GetAxisZ(),Up)>.999999);
                    if (Part.Kind==EPart::Display)
                    {
                        const FVector WorldPosition=Origin+Radial.RotateVector(Part.Position);
                        const FVector WorldUser=Origin+Radial.RotateVector(User);
                        const FVector ToUser=FVector::VectorPlaneProject(WorldUser-WorldPosition,Up).GetSafeNormal();
                        TestTrue(TEXT("Screen face points toward the seat on rotated planets"),FVector::DotProduct(-WorldRotation.GetAxisY(),ToUser)>.99999);
                    }
                }
                if (Seat && Back)
                {
                    const FVector WorldSeat=Origin+Radial.RotateVector(Seat->Position);
                    const FVector WorldBack=Origin+Radial.RotateVector(Back->Position);
                    const FVector ToTable=FVector::VectorPlaneProject(WorldTarget-WorldSeat,Up).GetSafeNormal();
                    TestTrue(TEXT("Backrest stays behind the sitter in radial world space"),FVector::DotProduct(WorldBack-WorldSeat,ToTable)<-20);
                }
            }
            // A validator that only accepts the generator output could miss the
            // original regression. Deliberately put the backrest toward the table.
            for (FPart& Part:Assembly.Parts)
                if (Part.Kind==EPart::Backrest) Part.Position+=Part.Rotation.GetAxisY()*56;
            TestFalse(TEXT("Reject the original chair facing regression"),Validate(Assembly,Reason));
        }
    }
    for (EPart Changed:{EPart::Display,EPart::Keyboard,EPart::Mouse})
    {
        FAssembly Broken=Workstation(Table,Table+FVector(0,-110,0),280);
        for (FPart& Part:Broken.Parts)
            if (Part.Kind==Changed)
            {
                if (Changed==EPart::Display) Part.Position.Y+=15;
                else Part.Position.Z+=5;
            }
        FString Reason;
        TestFalse(TEXT("Reject a rear-facing screen or floating tabletop item"),Validate(Broken,Reason));
    }
    for (double Level:{0.0,480.0,8160.0})
    {
        const FVector FloorTable=Table+FVector(0,0,Level);
        FAssembly Upper=Workstation(FloorTable,FloorTable+FVector(0,-110,0),280);
        FString Reason;
        TestTrue(TEXT("Upper-floor workstations retain tabletop support"),Validate(Upper,Reason));
        TestTrue(TEXT("Chair feet rest on their own floor"),FMath::IsNearlyEqual(Upper.Bounds.Min.Z,Level,.001));
    }
    for (double Side:{-1.0,1.0})
    {
        const FAssembly Diagnostics=DiagnosticDisplay(FVector(Side*700+110,420,0),FVector(0,340,0));
        FString Reason;
        TestTrue(TEXT("Clinic displays face the staff aisle from both sides"),Validate(Diagnostics,Reason));
    }
    return true;
}
#endif
