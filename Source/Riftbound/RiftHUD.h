#pragma once
#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "RiftHUD.generated.h"
UCLASS()
class RIFTBOUND_API ARiftHUD : public AHUD
{
    GENERATED_BODY()
public:
    virtual void DrawHUD() override;
};
