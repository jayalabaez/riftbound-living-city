#pragma once
#include "CoreMinimal.h"
#include "VoyagerItems.generated.h"

UENUM()
enum class EVoyagerItem : uint8 { RawMeat, CookedMeat, Hide, Bone, Medkit, DemolitionCharge, EnergyCell, Count };

namespace VoyagerItems
{
    inline const TCHAR* Name(EVoyagerItem Item)
    {
        static const TCHAR* Names[]={TEXT("Raw meat"),TEXT("Cooked meat"),TEXT("Hide"),TEXT("Bone"),TEXT("Medkit"),TEXT("Demolition charge"),TEXT("Energy cells")};
        const int32 Index=int32(Item);return Index>=0&&Index<int32(EVoyagerItem::Count)?Names[Index]:TEXT("Unknown item");
    }
    inline TArray<int32> StartingInventory(){TArray<int32> Result;Result.Init(0,int32(EVoyagerItem::Count));Result[int32(EVoyagerItem::EnergyCell)]=120;return Result;}
}
