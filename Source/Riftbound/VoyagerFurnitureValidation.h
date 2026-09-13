#pragma once

#include "VoyagerFurnitureLayout.h"

namespace VoyagerFurniture
{
    /** Geometry contract shared by the regression test and opt-in generated-city
     * audit. It observes presentation only; it cannot write simulation state. */
    inline bool Validate(const FAssembly& Assembly, FString& Reason)
    {
        const FPart* Seat=nullptr;
        const FPart* Back=nullptr;
        const FPart* Table=nullptr;
        const FPart* Display=nullptr;
        const FPart* Housing=nullptr;
        const FPart* Keyboard=nullptr;
        auto Fail=[&](const TCHAR* Why) { Reason=Why; return false; };
        if (!Assembly.Bounds.IsValid || Assembly.Parts.IsEmpty()) return Fail(TEXT("EMPTY_ASSEMBLY"));
        for (const FPart& Part:Assembly.Parts)
        {
            if (Part.Position.ContainsNaN() || Part.Size.ContainsNaN() || Part.Rotation.ContainsNaN() ||
                !Part.Rotation.IsNormalized() || Part.Size.GetMin()<=0)
                return Fail(TEXT("INVALID_PART_TRANSFORM"));
            if (Part.Kind==EPart::Seat) Seat=&Part;
            if (Part.Kind==EPart::Backrest) Back=&Part;
            if (Part.Kind==EPart::Tabletop) Table=&Part;
            if (Part.Kind==EPart::Display) Display=&Part;
            if (Part.Kind==EPart::MonitorHousing) Housing=&Part;
            if (Part.Kind==EPart::Keyboard) Keyboard=&Part;
        }
        if (Seat || Back)
        {
            if (!Seat || !Back) return Fail(TEXT("INCOMPLETE_CHAIR"));
            const FVector ToTable=FVector(Assembly.TableTarget.X-Seat->Position.X,
                Assembly.TableTarget.Y-Seat->Position.Y,0).GetSafeNormal();
            if (ToTable.IsNearlyZero() || FVector::DotProduct(Seat->Rotation.GetAxisY(),ToTable)<.999 ||
                FVector::DotProduct(Back->Position-Seat->Position,ToTable)>-20)
                return Fail(TEXT("CHAIR_FACES_AWAY_FROM_TABLE"));
        }
        if (Display || Housing)
        {
            if (!Display || !Housing) return Fail(TEXT("INCOMPLETE_DISPLAY"));
            const FVector ToUser=FVector(Assembly.SeatedUser.X-Display->Position.X,
                Assembly.SeatedUser.Y-Display->Position.Y,0).GetSafeNormal();
            if (FVector::DotProduct(-Display->Rotation.GetAxisY(),ToUser)<.999 ||
                FVector::DotProduct(Display->Position-Housing->Position,ToUser)<=Housing->Size.Y*.5 ||
                (Keyboard && FVector::DotProduct(Keyboard->Position-Display->Position,ToUser)<=20))
                return Fail(TEXT("SCREEN_OR_KEYBOARD_FACES_AWAY_FROM_USER"));
        }
        if (Table)
        {
            if (!Display || !Housing || !Keyboard) return Fail(TEXT("INCOMPLETE_WORKSTATION"));
            const double Top=Table->Position.Z+Table->Size.Z*.5;
            for (const FPart& Part:Assembly.Parts)
                if (Part.Kind==EPart::Keyboard || Part.Kind==EPart::Mouse)
                {
                    const FVector Relative=Table->Rotation.UnrotateVector(Part.Position-Table->Position);
                    if (FMath::Abs(Part.Position.Z-Part.Size.Z*.5-Top)>.05 ||
                        FMath::Abs(Relative.X)+Part.Size.X*.5>Table->Size.X*.5 ||
                        FMath::Abs(Relative.Y)+Part.Size.Y*.5>Table->Size.Y*.5)
                        return Fail(TEXT("UNSUPPORTED_DESKTOP_ITEM"));
                }
        }
        return true;
    }
}
