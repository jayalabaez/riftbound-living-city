#pragma once

#include "CoreMinimal.h"

/** Presentation geometry only. Every assembly uses +Y from its seated user toward
 * its table, +X across the tabletop and +Z above the floor. The building's radial
 * transform is applied once by the settlement presenter. */
namespace VoyagerFurniture
{
    enum class ESurface : uint8 { Wood, Metal, Fabric, Ceramic, Screen };
    enum class EPart : uint8 { Structure, Tabletop, Seat, Backrest, MonitorHousing, Display, Keyboard, Keys, Mouse };

    struct FPart
    {
        ESurface Surface;
        EPart Kind;
        FVector Position;
        FVector Size;
        FQuat Rotation;
    };

    struct FAssembly
    {
        TArray<FPart, TInlineAllocator<24>> Parts;
        FBox Bounds = FBox(ForceInit);
        FVector TableTarget = FVector::ZeroVector;
        FVector SeatedUser = FVector::ZeroVector;

        void Add(ESurface Surface, EPart Kind, const FTransform& Frame, FVector Offset, FVector Size)
        {
            const FVector Position = Frame.TransformPosition(Offset);
            const FQuat Rotation = Frame.GetRotation();
            Parts.Add({Surface, Kind, Position, Size, Rotation});
            const FVector Extent = (Rotation.GetAxisX().GetAbs() * Size.X +
                Rotation.GetAxisY().GetAbs() * Size.Y + Rotation.GetAxisZ().GetAbs() * Size.Z) * .5;
            Bounds += Position - Extent;
            Bounds += Position + Extent;
        }
    };

    inline FTransform FacingTable(FVector Origin, FVector SeatedUser, FVector Table)
    {
        const FVector Forward = FVector(Table.X-SeatedUser.X, Table.Y-SeatedUser.Y, 0).GetSafeNormal();
        const FVector Right = FVector::CrossProduct(Forward, FVector::UpVector);
        return FTransform(FRotationMatrix::MakeFromXY(Right, Forward).ToQuat(), Origin);
    }

    inline void AddChair(FAssembly& Assembly, FVector SeatedUser, FVector Table)
    {
        const FTransform Frame = FacingTable(SeatedUser, SeatedUser, Table);
        Assembly.Add(ESurface::Fabric, EPart::Seat, Frame, FVector(0,0,48), FVector(64,64,16));
        // A backrest belongs behind the sitter, away from the associated table.
        Assembly.Add(ESurface::Fabric, EPart::Backrest, Frame, FVector(0,-28,80), FVector(64,10,64));
        for (int32 X : {-1,1}) for (int32 Y : {-1,1})
            Assembly.Add(ESurface::Metal, EPart::Structure, Frame, FVector(X*24,Y*24,20), FVector(6,6,40));
    }

    inline FAssembly Chair(FVector SeatedUser, FVector Table)
    {
        FAssembly Assembly;
        Assembly.TableTarget = Table;
        Assembly.SeatedUser = SeatedUser;
        AddChair(Assembly, SeatedUser, Table);
        return Assembly;
    }

    inline FAssembly Workstation(FVector Table, FVector SeatedUser, double Width, bool bWithChair = true)
    {
        FAssembly Assembly;
        Assembly.TableTarget = Table;
        Assembly.SeatedUser = SeatedUser;
        const FTransform Frame = FacingTable(Table, SeatedUser, Table);
        constexpr double Top = 91;
        Assembly.Add(ESurface::Wood, EPart::Tabletop, Frame, FVector(0,0,86), FVector(Width,115,10));
        for (int32 Side : {-1,1})
            Assembly.Add(ESurface::Metal, EPart::Structure, Frame,
                FVector(Side*(Width*.5-12),0,40.5), FVector(16,92,81));
        // The stand, keyboard and mouse touch the same tabletop. The monitor has
        // an opaque rear housing; only its face toward the sitter emits light.
        Assembly.Add(ESurface::Metal, EPart::Structure, Frame, FVector(0,22,Top+1.5), FVector(34,28,3));
        Assembly.Add(ESurface::Metal, EPart::Structure, Frame, FVector(0,22,106), FVector(8,8,28));
        Assembly.Add(ESurface::Metal, EPart::MonitorHousing, Frame, FVector(0,26,130), FVector(86,8,54));
        Assembly.Add(ESurface::Screen, EPart::Display, Frame, FVector(0,21.6,130), FVector(80,.8,48));
        Assembly.Add(ESurface::Metal, EPart::Keyboard, Frame, FVector(0,-25,Top+1.5), FVector(62,26,3));
        Assembly.Add(ESurface::Ceramic, EPart::Keys, Frame, FVector(0,-25,Top+3.4), FVector(55,20,.8));
        Assembly.Add(ESurface::Metal, EPart::Mouse, Frame, FVector(44,-25,Top+2), FVector(9,15,4));
        if (bWithChair) AddChair(Assembly, SeatedUser, Table);
        return Assembly;
    }

    inline FAssembly DiagnosticDisplay(FVector Position, FVector Observer)
    {
        FAssembly Assembly;
        Assembly.TableTarget=Position;
        Assembly.SeatedUser=Observer;
        const FTransform Frame=FacingTable(Position,Observer,Position);
        Assembly.Add(ESurface::Metal,EPart::Structure,Frame,FVector(0,0,2),FVector(45,45,4));
        Assembly.Add(ESurface::Metal,EPart::Structure,Frame,FVector(0,0,85),FVector(8,8,170));
        Assembly.Add(ESurface::Metal,EPart::MonitorHousing,Frame,FVector(0,0,166),FVector(59,8,42));
        Assembly.Add(ESurface::Screen,EPart::Display,Frame,FVector(0,-4.4,166),FVector(55,.8,38));
        return Assembly;
    }
}
