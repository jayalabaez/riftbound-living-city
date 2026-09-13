#pragma once
#include "CoreMinimal.h"
class AVoyagerShip;
namespace VoyagerLanding {
// Read-only authority queries; these functions never move actors or change possession.
bool FindLanding(const AVoyagerShip* Ship,FVector& Location,FRotator& Rotation,FString& Reason);
bool FindExit(const AVoyagerShip* Ship,const FVector& Location,const FRotator& Rotation,FVector& Exit);
}
