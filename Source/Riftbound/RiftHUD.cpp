#include "RiftHUD.h"
#include "RiftCharacter.h"
#include "RiftGameMode.h"
#include "RiftWorld.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/PlayerState.h"

void ARiftHUD::DrawHUD()
{
    Super::DrawHUD();if(!Canvas)return;
    ARiftCharacter* P=Cast<ARiftCharacter>(GetOwningPawn());
    ARiftGameState* GS=GetWorld()->GetGameState<ARiftGameState>();
    if(!P||!GS)return;
    const float W=Canvas->SizeX,H=Canvas->SizeY,S=FMath::Min(W/1600.f,H/900.f);
    const FLinearColor White(.86f,.94f,.92f),Muted(.43f,.62f,.61f),Mint(.25f,1.f,.77f),Amber(1,.69f,.27f),Dark(.008f,.022f,.029f,.87f);
    auto Rect=[&](float X,float Y,float A,float B,FLinearColor C){DrawRect(C,X*S,Y*S,A*S,B*S);};
    auto Text=[&](const FString& T,float X,float Y,float Size,FLinearColor C){DrawText(T,C,X*S,Y*S,GEngine->GetMediumFont(),Size*S*1.8f,false);};
    const float VW=W/S,VH=H/S;
    Rect(30,28,3,54,Mint);Text(TEXT("R I F T B O U N D"),48,27,1.35f,White);Text(TEXT("FIELD EXPEDITION  /  BLACKPINE RESERVE"),48,61,.6f,Muted);
    FString Objective=TEXT("INVESTIGATE THE VISITOR");
    FString Detail=TEXT("Shoot the spacecraft to open the anomaly.");
    if(GS->Phase==1){Objective=TEXT("REALITY IS BREAKING");Detail=TEXT("Stay together. The first breach is forming.");}
    if(GS->Phase==2){Objective=TEXT("HOLD THE CLEARING");Detail=FString::Printf(TEXT("Eliminate the horde  /  %d hostiles remain"),GS->Remaining);}
    if(GS->Phase==3){Objective=TEXT("BREACH CONTAINED");Detail=FString::Printf(TEXT("Supplies restored. Next anomaly in %02d seconds."),FMath::CeilToInt(GS->PhaseTime));}
    if(GS->Phase==4){Objective=TEXT("REALITY RESTORED");Detail=TEXT("You survived all three anomalies. Press ENTER for another expedition.");}
    if(GS->Phase==5){Objective=TEXT("EXPEDITION LOST");Detail=TEXT("The anomaly consumed the team. Press ENTER to try again.");}
    Rect(30,109,460,75,Dark);Rect(30,109,460,2,FLinearColor(.11f,.36f,.32f,.8f));
    Text(Objective,47,122,.83f,Mint);Text(Detail,47,155,.58f,White);
    const TCHAR* Anomalies[]={TEXT("UNKNOWN SIGNAL"),TEXT("01 / EXTRATERRESTRIAL"),TEXT("02 / WAR ECHO"),TEXT("03 / PRIMAL RUPTURE")};
    Text(Anomalies[FMath::Clamp(GS->Wave,0,3)],VW-299,31,.72f,GS->Wave==0?Muted:Amber);
    Text(FString::Printf(TEXT("SQUAD  %d / 4     KILLS  %02d"),GS->PlayerArray.Num(),GS->TeamKills),VW-299,60,.64f,White);
    Text(FString::Printf(TEXT("WAVE  %d / 3"),GS->Wave),VW-299,86,.7f,Mint);
    // A restrained reticle and positive hit confirmation.
    const float CX=W*.5f,CY=H*.5f;const FLinearColor Ret=P->HitFeedback>0?Amber:White;
    DrawLine(CX-12*S,CY,CX-5*S,CY,Ret,1.5f*S);DrawLine(CX+5*S,CY,CX+12*S,CY,Ret,1.5f*S);
    DrawLine(CX,CY-12*S,CX,CY-5*S,Ret,1.5f*S);DrawLine(CX,CY+5*S,CX,CY+12*S,Ret,1.5f*S);
    if(P->HitFeedback>0){DrawLine(CX-17*S,CY-17*S,CX-10*S,CY-10*S,Amber,2*S);DrawLine(CX+10*S,CY+10*S,CX+17*S,CY+17*S,Amber,2*S);}
    FHitResult Hit;FCollisionQueryParams Q;Q.AddIgnoredActor(P);
    if(GetWorld()->LineTraceSingleByChannel(Hit,P->Camera->GetComponentLocation(),P->Camera->GetComponentLocation()+P->Camera->GetForwardVector()*10000,ECC_Visibility,Q))
    {
        if(auto UFO=Cast<ARiftUFO>(Hit.GetActor()))if(!UFO->bActivated){Text(TEXT("UNIDENTIFIED CRAFT"),VW/2-85,VH/2+40,.62f,Mint);Rect(VW/2-80,VH/2+65,160,3,Dark);Rect(VW/2-80,VH/2+65,160*FMath::Clamp(UFO->Health/180.f,0.f,1.f),3,Mint);}
        if(Cast<ARiftProp>(Hit.GetActor()))Text(TEXT("BREAKABLE COVER"),VW/2-70,VH/2+40,.55f,Amber);
    }
    Rect(30,VH-140,300,100,Dark);Text(TEXT("VITALS"),48,VH-124,.56f,Muted);
    Text(FString::Printf(TEXT("%03d"),FMath::CeilToInt(P->Health)),48,VH-105,1.8f,P->Health<30?Amber:White);
    Text(TEXT("/ 100"),142,VH-82,.68f,Muted);Rect(48,VH-57,260,5,FLinearColor(.12f,.2f,.22f));Rect(48,VH-57,260*P->Health/100,5,P->Health<30?Amber:Mint);
    Rect(48,VH-45,260*P->Stamina/100,2,Muted);
    Rect(VW-280,VH-140,250,100,Dark);Text(TEXT("MX-4  /  PULSE RIFLE"),VW-261,VH-124,.6f,Muted);
    Text(P->bReloading?TEXT("RELOAD"):FString::Printf(TEXT("%02d"),P->Ammo),VW-261,VH-103,P->bReloading?1.1f:2.f,White);
    if(!P->bReloading)Text(TEXT("/ 30"),VW-178,VH-78,.75f,Muted);
    Text(FString::Printf(TEXT("G  PULSE CHARGES   %d"),P->Grenades),VW-260,VH-55,.65f,Amber);
    Text(TEXT("WASD  MOVE     SHIFT  SPRINT     SPACE  JUMP     RMB  AIM     R  RELOAD     G  CHARGE     ESC  CO-OP / MENU"),VW/2-390,VH-25,.52f,Muted);
    if(GS->Phase==1 || GS->Phase>=4)
    {
        Rect(VW/2-365,VH*.26f,730,102,Dark);Text(Objective,VW/2-220,VH*.26f+19,1.75f,GS->Phase==1?Amber:Mint);
        Text(Detail,VW/2-275,VH*.26f+69,.65f,White);
    }
    if(P->Health<=0){Rect(VW/2-230,VH/2-110,460,70,Dark);Text(FString::Printf(TEXT("RECOVERING  /  %d SEC"),FMath::CeilToInt(P->RespawnRemaining)),VW/2-180,VH/2-92,1.2f,Amber);}
    if(P->DamageFeedback>0){DrawRect(FLinearColor(1,.03f,.01f,P->DamageFeedback*.30f),0,0,W,H);}
}
