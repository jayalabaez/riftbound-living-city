#include "RiftCharacter.h"
#include "RiftVisual.h"
#include "RiftGameMode.h"
#include "RiftEnemy.h"
#include "RiftWorld.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InputComponent.h"
#include "Components/SpotLightComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "DrawDebugHelpers.h"
#include "Sound/SoundBase.h"
#include "TimerManager.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/SCompoundWidget.h"
#include "Misc/CommandLine.h"

class SRiftMenuFrame : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SRiftMenuFrame) {}
        SLATE_DEFAULT_SLOT(FArguments, Content)
        SLATE_EVENT(FSimpleDelegate, OnClose)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args){Close=Args._OnClose;ChildSlot[Args._Content.Widget];}
    virtual bool SupportsKeyboardFocus() const override{return true;}
    virtual FReply OnKeyDown(const FGeometry& Geometry,const FKeyEvent& Event) override
    {if(Event.GetKey()==EKeys::Escape){Close.ExecuteIfBound();return FReply::Handled();}return SCompoundWidget::OnKeyDown(Geometry,Event);}
    FSimpleDelegate Close;
};

ARiftCharacter::ARiftCharacter()
{
    PrimaryActorTick.bCanEverTick = true;
    bReplicates = true;
    GetCapsuleComponent()->InitCapsuleSize(34.f, 88.f);
    GetCharacterMovement()->MaxWalkSpeed = 460.f;
    GetCharacterMovement()->JumpZVelocity = 510.f;
    GetCharacterMovement()->AirControl = .32f;
    Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Eyes"));
    Camera->SetupAttachment(GetCapsuleComponent());
    Camera->SetRelativeLocation(FVector(0, 0, 64));
    Camera->bUsePawnControlRotation = true;
    Camera->FieldOfView = 92.f;
    Weapon = CreateDefaultSubobject<USceneComponent>(TEXT("Weapon"));
    Weapon->SetupAttachment(Camera);
    Weapon->SetRelativeLocation(FVector(38, 18, -18));
    Weapon->SetRelativeScale3D(FVector(.62f));
    Flashlight = CreateDefaultSubobject<USpotLightComponent>(TEXT("Flashlight"));
    Flashlight->SetupAttachment(Camera);
    Flashlight->SetRelativeLocation(FVector(30, 10, -10));
    Flashlight->SetIntensity(2800);
    Flashlight->SetAttenuationRadius(2300);
    Flashlight->SetInnerConeAngle(12);
    Flashlight->SetOuterConeAngle(30);
    Flashlight->SetLightColor(FLinearColor(.65f, .83f, 1.f));
    Flashlight->SetCastShadows(false);
    SetNetUpdateFrequency(50.f);
}

void ARiftCharacter::BeginPlay()
{
    Super::BeginPlay();
    const FLinearColor Metal(.035f,.065f,.08f), Grip(.09f,.11f,.12f), Mint(.08f,.9f,.72f);
    auto Gun = [this](FName N, const TCHAR* Shape, FVector P, FVector S, FLinearColor C, bool Glow=false)
    {
        UStaticMeshComponent* M = RiftVisual::Mesh(this, Weapon, N, Shape, P, S, C, Glow);
        M->SetOnlyOwnerSee(true); M->SetCastShadow(false); return M;
    };
    Gun(TEXT("Receiver"),TEXT("Cube"),FVector(0,0,0),FVector(.52f,.10f,.13f),Metal);
    Gun(TEXT("UpperRail"),TEXT("Cube"),FVector(5,0,8),FVector(.48f,.065f,.025f),Grip);
    Gun(TEXT("Barrel"),TEXT("Cube"),FVector(39,0,1),FVector(.32f,.045f,.05f),Metal);
    Gun(TEXT("Muzzle"),TEXT("Cube"),FVector(55,0,1),FVector(.09f,.07f,.07f),Grip);
    Gun(TEXT("Magazine"),TEXT("Cube"),FVector(0,0,-13),FVector(.115f,.075f,.23f),Grip)->SetRelativeRotation(FRotator(-12,0,0));
    Gun(TEXT("Stock"),TEXT("Cube"),FVector(-33,0,-2),FVector(.22f,.10f,.14f),Metal);
    Gun(TEXT("SightFront"),TEXT("Cube"),FVector(26,0,12),FVector(.04f,.045f,.09f),Grip);
    Gun(TEXT("SightGlow"),TEXT("Sphere"),FVector(26,0,17),FVector(.023f),Mint,true);
    Gun(TEXT("EnergyStripe"),TEXT("Cube"),FVector(2,-5.2f,2),FVector(.30f,.009f,.025f),Mint,true);
    Gun(TEXT("HandLeft"),TEXT("Cube"),FVector(15,-2,-10),FVector(.16f,.11f,.10f),FLinearColor(.17f,.23f,.23f));
    Gun(TEXT("Forearm"),TEXT("Cube"),FVector(-10,4,-17),FVector(.3f,.105f,.12f),FLinearColor(.13f,.20f,.20f));
    UStaticMeshComponent* Body=RiftVisual::Mesh(this,GetRootComponent(),TEXT("ExplorerBody"),TEXT("Cylinder"),FVector(0,0,-20),FVector(.52f,.52f,.9f),FLinearColor(.09f,.25f,.26f));
    Body->SetOwnerNoSee(true);
    UStaticMeshComponent* Head=RiftVisual::Mesh(this,GetRootComponent(),TEXT("ExplorerHelmet"),TEXT("Sphere"),FVector(0,0,48),FVector(.44f),Grip);
    Head->SetOwnerNoSee(true);
    UStaticMeshComponent* Visor=RiftVisual::Mesh(this,GetRootComponent(),TEXT("ExplorerVisor"),TEXT("Cube"),FVector(19,0,49),FVector(.07f,.34f,.12f),Mint,true);
    Visor->SetOwnerNoSee(true);
    for(int I=-1;I<=1;I+=2)
    {
        auto Leg=RiftVisual::Mesh(this,GetRootComponent(),FName(*FString::Printf(TEXT("Leg%d"),I)),TEXT("Cube"),FVector(0,I*15,-64),FVector(.18f,.20f,.46f),Grip);
        Leg->SetOwnerNoSee(true);
    }
    Flashlight->SetVisibility(IsLocallyControlled());
    UE_LOG(LogTemp,Display,TEXT("RIFT PLAYER begin authority=%d local=%d"),HasAuthority(),IsLocallyControlled());
}

void ARiftCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(ARiftCharacter,Health); DOREPLIFETIME(ARiftCharacter,Ammo);
    DOREPLIFETIME(ARiftCharacter,Grenades); DOREPLIFETIME(ARiftCharacter,Kills);
    DOREPLIFETIME(ARiftCharacter,bReloading); DOREPLIFETIME(ARiftCharacter,RespawnRemaining);
    DOREPLIFETIME(ARiftCharacter,Stamina);
}

void ARiftCharacter::SetupPlayerInputComponent(UInputComponent* I)
{
    Super::SetupPlayerInputComponent(I);
    I->BindAxis("MoveForward",this,&ARiftCharacter::Forward); I->BindAxis("MoveRight",this,&ARiftCharacter::Right);
    I->BindAxis("Turn",this,&ARiftCharacter::Turn); I->BindAxis("LookUp",this,&ARiftCharacter::Look);
    I->BindAction("Jump",IE_Pressed,this,&ACharacter::Jump); I->BindAction("Jump",IE_Released,this,&ACharacter::StopJumping);
    I->BindAction("Fire",IE_Pressed,this,&ARiftCharacter::StartFire); I->BindAction("Fire",IE_Released,this,&ARiftCharacter::StopFire);
    I->BindAction("Aim",IE_Pressed,this,&ARiftCharacter::Aim); I->BindAction("Aim",IE_Released,this,&ARiftCharacter::UnAim);
    I->BindAction("Reload",IE_Pressed,this,&ARiftCharacter::Reload); I->BindAction("Grenade",IE_Pressed,this,&ARiftCharacter::Grenade);
    I->BindAction("Sprint",IE_Pressed,this,&ARiftCharacter::Sprint); I->BindAction("Sprint",IE_Released,this,&ARiftCharacter::StopSprint);
    I->BindAction("Interact",IE_Pressed,this,&ARiftCharacter::Interact); I->BindAction("Restart",IE_Pressed,this,&ARiftCharacter::RestartExpedition);
    I->BindAction("Menu",IE_Pressed,this,&ARiftCharacter::Menu);
}
void ARiftCharacter::Forward(float V){ if(Health>0) AddMovementInput(GetActorForwardVector(),V); }
void ARiftCharacter::Right(float V){ if(Health>0) AddMovementInput(GetActorRightVector(),V); }
void ARiftCharacter::Turn(float V){ AddControllerYawInput(V*.75f); }
void ARiftCharacter::Look(float V){ AddControllerPitchInput(V*.75f); }
void ARiftCharacter::StartFire(){bFiring=true;}
void ARiftCharacter::StopFire(){bFiring=false;}
void ARiftCharacter::Aim(){bAiming=true;}
void ARiftCharacter::UnAim(){bAiming=false;}
void ARiftCharacter::Reload(){ServerReload();}
void ARiftCharacter::Grenade(){ServerGrenade(Camera->GetForwardVector());}
void ARiftCharacter::Sprint(){bSprintRequested=true;GetCharacterMovement()->MaxWalkSpeed=730;ServerSprint(true);}
void ARiftCharacter::StopSprint(){bSprintRequested=false;GetCharacterMovement()->MaxWalkSpeed=460;ServerSprint(false);}
void ARiftCharacter::Interact(){ServerReload();}
void ARiftCharacter::RestartExpedition(){ServerRestart();}
void ARiftCharacter::Menu(){if(auto PC=Cast<ARiftPlayerController>(Controller)) PC->ToggleMenu();}
void ARiftCharacter::ServerSprint_Implementation(bool Value){bSprintRequested=Value;}

void ARiftCharacter::Tick(float D)
{
    Super::Tick(D);
    HitFeedback=FMath::Max(0.f,HitFeedback-D); DamageFeedback=FMath::Max(0.f,DamageFeedback-D);
    if(HasAuthority())
    {
        if(Health<=0)
        {
            RespawnRemaining=FMath::Max(0.f,RespawnRemaining-D);
            if(RespawnRemaining<=0){SetActorLocation(FVector(-1800,FMath::RandRange(-240,240),130));Refill();GetCharacterMovement()->SetMovementMode(MOVE_Walking);}
        }
        Stamina=FMath::Clamp(Stamina+(bSprintRequested && GetVelocity().Size2D()>20 ? -22.f : 16.f)*D,0.f,100.f);
        GetCharacterMovement()->MaxWalkSpeed=(bSprintRequested && Stamina>1 && Health>0)?730.f:460.f;
        if(bReloading){ReloadRemaining-=D;if(ReloadRemaining<=0){Ammo=30;bReloading=false;}}
        if(Health>0 && Health<100 && GetWorld()->TimeSeconds-LastDamageTime>9.f)Health=FMath::Min(100.f,Health+D*3.f);
    }
    if(IsLocallyControlled())
    {
        auto PC=Cast<ARiftPlayerController>(Controller);
        if(PC && PC->bMenuVisible){bFiring=false;bAiming=false;}
        if(bFiring && Health>0 && !bReloading && GetWorld()->TimeSeconds>=NextLocalShot)
        {
            NextLocalShot=GetWorld()->TimeSeconds+.12f;
            if(Ammo>0){ServerFire(Camera->GetForwardVector());AddControllerPitchInput(-.12f);}
            else ServerReload();
        }
        Camera->SetFieldOfView(FMath::FInterpTo(Camera->FieldOfView,bAiming?65.f:92.f,D,12));
        BobTime+=D*FMath::Clamp(GetVelocity().Size2D()/45.f,0.f,15.f);
        const FVector Desired=bAiming?FVector(30,0,-14):FVector(38,18,-18);
        const float Bob=FMath::Sin(BobTime)*FMath::Min(GetVelocity().Size2D()/460.f,1.f);
        Weapon->SetRelativeLocation(FMath::VInterpTo(Weapon->GetRelativeLocation(),Desired+FVector(0,Bob*.4f,Bob*.5f),D,13));
        Weapon->SetRelativeRotation(FRotator(bReloading?-25:0,0,bReloading?25:0));
        Flashlight->SetVisibility(true);
        if(!HasAuthority())GetCharacterMovement()->MaxWalkSpeed=(bSprintRequested&&Stamina>1)?730:460;
    }
}

void ARiftCharacter::ServerFire_Implementation(FVector_NetQuantizeNormal Direction)
{
    const float Now=GetWorld()->TimeSeconds;
    if(Health<=0 || bReloading || Ammo<=0 || Now-ShotTime<.105f || Direction.ContainsNaN())return;
    // View rotation can arrive after a reliable shot RPC during a fast mouse turn.
    // Validate the aim vector; derive origin, range, cadence and damage on authority.
    if(!FMath::IsNearlyEqual(Direction.SizeSquared(),1.f,.02f))return;
    ShotTime=Now;--Ammo;
    const FVector Start=GetActorLocation()+FVector(0,0,64);
    FHitResult Hit;FCollisionQueryParams Q(SCENE_QUERY_STAT(RiftShot),true,this);
    const bool bHit=GetWorld()->LineTraceSingleByChannel(Hit,Start,Start+Direction*14000,ECC_Visibility,Q);
    AActor* Target=Hit.GetActor();
    if(Target && (Target->IsA<ARiftEnemy>()||Target->IsA<ARiftProp>()||Target->IsA<ARiftUFO>()||Target->IsA<ARiftMine>()))
    {
        const float Applied=UGameplayStatics::ApplyPointDamage(Target,26.f,Direction,Hit,Controller,this,nullptr);
        if(Applied>0)ConfirmHit();
    }
    ShotEffects(bHit?Hit.ImpactPoint:Start+Direction*14000,bHit);
}
void ARiftCharacter::ServerReload_Implementation()
{
    if(Health<=0||bReloading||Ammo>=30)return;
    bReloading=true;ReloadRemaining=1.45f;
}
void ARiftCharacter::ServerGrenade_Implementation(FVector_NetQuantizeNormal Direction)
{
    if(Health<=0||Grenades<=0||Direction.ContainsNaN())return;
    if(!FMath::IsNearlyEqual(Direction.SizeSquared(),1.f,.02f))return;
    --Grenades;
    FVector Start=GetActorLocation()+FVector(0,0,55);
    FHitResult Hit;FCollisionQueryParams Q(SCENE_QUERY_STAT(RiftGrenade),false,this);
    FVector At=Start+Direction*1500;
    if(GetWorld()->LineTraceSingleByChannel(Hit,Start,At,ECC_Visibility,Q))At=Hit.ImpactPoint+Hit.ImpactNormal*20;
    // A thrown pulse charge detonates at the aimed impact point after a short fuse.
    DrawDebugLine(GetWorld(),Start,At,FColor::Orange,false,.65f,0,5);
    FTimerHandle Fuse;
    GetWorldTimerManager().SetTimer(Fuse,FTimerDelegate::CreateWeakLambda(this,[this,At](){Explode(At);}),.65f,false);
}
void ARiftCharacter::Explode(FVector At)
{
    ExplosionEffects(At);
    for(TActorIterator<AActor> It(GetWorld());It;++It)
    {
        AActor* A=*It;
        if(A==this || A->IsA<ARiftCharacter>())continue;
        const float Distance=FVector::Distance(At,A->GetActorLocation());
        if(Distance<620 && (A->IsA<ARiftEnemy>()||A->IsA<ARiftProp>()||A->IsA<ARiftUFO>()||A->IsA<ARiftMine>()))
            UGameplayStatics::ApplyDamage(A,220.f*(1.f-Distance/850.f),Controller,this,nullptr);
    }
}
void ARiftCharacter::ShotEffects_Implementation(FVector_NetQuantize End,bool bHit)
{
    if(GetNetMode()==NM_DedicatedServer)return;
    FVector Start=IsLocallyControlled()?Weapon->GetComponentLocation()+Camera->GetForwardVector()*55:GetActorLocation()+FVector(0,0,55);
    DrawDebugLine(GetWorld(),Start,End,FColor(100,255,218),false,.055f,0,1.8f);
    if(USoundBase* S=LoadObject<USoundBase>(nullptr,TEXT("/Game/Audio/S_Gun.S_Gun")))UGameplayStatics::PlaySoundAtLocation(this,S,Start,.33f,FMath::FRandRange(.93f,1.06f));
    if(bHit){auto FX=GetWorld()->SpawnActorDeferred<ARiftBurst>(ARiftBurst::StaticClass(),FTransform(FRotator::ZeroRotator,End));if(FX){FX->Size=.3f;UGameplayStatics::FinishSpawningActor(FX,FX->GetActorTransform());}}
}
void ARiftCharacter::ExplosionEffects_Implementation(FVector_NetQuantize At)
{
    if(GetNetMode()==NM_DedicatedServer)return;
    auto FX=GetWorld()->SpawnActorDeferred<ARiftBurst>(ARiftBurst::StaticClass(),FTransform(FRotator::ZeroRotator,At));
    if(FX){FX->Size=4.f;FX->Color=FLinearColor(1,.25f,.035f);UGameplayStatics::FinishSpawningActor(FX,FX->GetActorTransform());}
    if(USoundBase* S=LoadObject<USoundBase>(nullptr,TEXT("/Game/Audio/S_Explosion.S_Explosion")))UGameplayStatics::PlaySoundAtLocation(this,S,At,.7f);
}
void ARiftCharacter::ConfirmHit_Implementation(){HitFeedback=.16f;}
void ARiftCharacter::ConfirmDamage_Implementation(){DamageFeedback=.45f;}
float ARiftCharacter::TakeDamage(float Damage,const FDamageEvent& Event,AController* DamageInstigator,AActor* Causer)
{
    if(!HasAuthority()||Health<=0||Damage<=0)return 0;
    Health=FMath::Max(0.f,Health-Damage);LastDamageTime=GetWorld()->TimeSeconds;ConfirmDamage();
    if(Health<=0){RespawnRemaining=6.f;bReloading=false;GetCharacterMovement()->DisableMovement();UE_LOG(LogTemp,Display,TEXT("RIFT PLAYER DOWN; respawn in 6s"));}
    return Damage;
}
void ARiftCharacter::Refill(){if(!HasAuthority())return;Health=100;Ammo=30;Grenades=3;Stamina=100;bReloading=false;RespawnRemaining=0;GetCharacterMovement()->SetMovementMode(MOVE_Walking);}
void ARiftCharacter::AddKill(){if(HasAuthority())++Kills;}
void ARiftCharacter::ServerRestart_Implementation()
{
    if(auto GS=GetWorld()->GetGameState<ARiftGameState>())if(GS->Phase<4)return;
    if(auto GM=GetWorld()->GetAuthGameMode<ARiftGameMode>())GM->RestartRun();
}

ARiftBurst::ARiftBurst(){PrimaryActorTick.bCanEverTick=true;RootComponent=CreateDefaultSubobject<USceneComponent>(TEXT("Root"));}
void ARiftBurst::BeginPlay()
{
    Super::BeginPlay();SetLifeSpan(.7f);
    for(int I=0;I<8;++I){auto M=RiftVisual::Mesh(this,RootComponent,FName(*FString::Printf(TEXT("Spark%d"),I)),TEXT("Cube"),FVector::ZeroVector,FVector(.035f*Size),Color,true);Shards.Add(M);Velocities.Add(FMath::VRand()*FMath::FRandRange(100.f,360.f)*Size);}
}
void ARiftBurst::Tick(float D)
{
    Super::Tick(D);Age+=D;
    for(int I=0;I<Shards.Num();++I){Shards[I]->AddLocalOffset(Velocities[I]*D);Velocities[I].Z-=400*D;Shards[I]->SetRelativeScale3D(FVector(.035f*Size*FMath::Max(0.f,1.f-Age/.7f)));}
}

void ARiftPlayerController::BeginPlay()
{
    Super::BeginPlay();
    if(IsLocalController()){SetInputMode(FInputModeGameOnly());bShowMouseCursor=false;}
}
void ARiftPlayerController::EndPlay(const EEndPlayReason::Type Reason){HideMenu();Super::EndPlay(Reason);}
void ARiftPlayerController::ToggleMenu(){if(bMenuVisible)HideMenu();else ShowMenu();}
void ARiftPlayerController::HideMenu()
{
    if(MenuWidget.IsValid()&&GEngine&&GEngine->GameViewport)GEngine->GameViewport->RemoveViewportWidgetContent(MenuWidget.ToSharedRef());
    MenuWidget.Reset();AddressBox.Reset();bMenuVisible=false;bShowMouseCursor=false;
    if(IsLocalController())SetInputMode(FInputModeGameOnly());
}
void ARiftPlayerController::ShowMenu()
{
    if(!IsLocalController()||!GEngine||!GEngine->GameViewport)return;
    bMenuVisible=true;bShowMouseCursor=true;FlushPressedKeys();
    auto Label=[](const TCHAR* S,int Size){return SNew(STextBlock).Text(FText::FromString(S)).Font(FCoreStyle::GetDefaultFontStyle("Bold",Size)).ColorAndOpacity(FLinearColor(.82f,.95f,.92f));};
    TSharedRef<SWidget> Overlay=SNew(SOverlay)
    +SOverlay::Slot()[SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).BorderBackgroundColor(FLinearColor(.005f,.012f,.018f,.94f))]
    +SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
    [SNew(SBox).WidthOverride(480)
      [SNew(SVerticalBox)
       +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Label(TEXT("R I F T B O U N D"),36)]
       +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,28)[Label(TEXT("FOUR EXPLORERS. ONE BROKEN REALITY."),11)]
       +SVerticalBox::Slot().AutoHeight().Padding(0,5)[SNew(SButton).ContentPadding(16).OnClicked_Lambda([this](){HideMenu();return FReply::Handled();})[Label(TEXT("CONTINUE EXPLORING"),16)]]
       +SVerticalBox::Slot().AutoHeight().Padding(0,5)[SNew(SButton).ContentPadding(16).OnClicked_Lambda([this](){HideMenu();UGameplayStatics::OpenLevel(this,TEXT("/Game/Maps/Forest"),true,TEXT("listen"));return FReply::Handled();})[Label(TEXT("HOST A NEW CO-OP RUN"),16)]]
       +SVerticalBox::Slot().AutoHeight().Padding(0,18,0,4)[Label(TEXT("JOIN A FRIEND  /  host IP address"),12)]
       +SVerticalBox::Slot().AutoHeight().Padding(0,5)[SAssignNew(AddressBox,SEditableTextBox).Text(FText::FromString(TEXT("127.0.0.1"))).Font(FCoreStyle::GetDefaultFontStyle("Regular",18))]
       +SVerticalBox::Slot().AutoHeight().Padding(0,5)[SNew(SButton).ContentPadding(16).OnClicked_Lambda([this](){FString Address=AddressBox->GetText().ToString().TrimStartAndEnd();bool Valid=!Address.IsEmpty();for(TCHAR C:Address){if(!FChar::IsAlnum(C)&&C!=TEXT('.')&&C!=TEXT(':')&&C!=TEXT('-'))Valid=false;}if(Valid){HideMenu();ClientTravel(Address,TRAVEL_Absolute);}return FReply::Handled();})[Label(TEXT("CONNECT"),16)]]
       +SVerticalBox::Slot().AutoHeight().Padding(0,12)[Label(TEXT("1-4 players / LAN or reachable host / UDP 7777"),11)]
       +SVerticalBox::Slot().AutoHeight().Padding(0,5)[SNew(SButton).ContentPadding(12).OnClicked_Lambda([this](){ConsoleCommand(TEXT("quit"));return FReply::Handled();})[Label(TEXT("QUIT TO DESKTOP"),12)]]
       +SVerticalBox::Slot().AutoHeight().Padding(0,22,0,0)[Label(TEXT("WASD move  /  Mouse aim  /  LMB fire  /  R reload\nShift sprint  /  Space jump  /  G pulse charge\nShoot the spacecraft to begin. Break timber for escape routes."),11)]
      ]
    ];
    MenuWidget=SNew(SRiftMenuFrame).OnClose_Lambda([this](){HideMenu();})[Overlay];
    GEngine->GameViewport->AddViewportWidgetContent(MenuWidget.ToSharedRef(),100);
    FInputModeUIOnly Mode;Mode.SetWidgetToFocus(MenuWidget);Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);SetInputMode(Mode);
}

void ARiftPlayerController::Tick(float D)
{
    Super::Tick(D);
    if(!IsLocalController() && GetNetMode()!=NM_DedicatedServer)return;
    if(!FParse::Param(FCommandLine::Get(),TEXT("RiftProbe")))return;
    ProbeTime+=D;
    if(FParse::Param(FCommandLine::Get(),TEXT("RiftCombatTest")))
    {
        if(auto P=Cast<ARiftCharacter>(GetPawn()))
        {
            auto State=GetWorld()->GetGameState<ARiftGameState>();
            if(ProbeTime>2 && ProbeTime<5 && State && State->Phase==0)
            {
                for(TActorIterator<ARiftUFO> It(GetWorld());It;++It)
                {
                    FVector AimDirection=(It->GetActorLocation()-P->Camera->GetComponentLocation()).GetSafeNormal();
                    SetControlRotation(AimDirection.Rotation());P->ServerFire(AimDirection);break;
                }
            }
            if(State&&State->Phase==1&&ClientTestStage==0){UE_LOG(LogTemp,Display,TEXT("RIFT COMBAT TEST PASS UFO_RIFLE"));ClientTestStage=1;}
            if(State&&State->Phase==2)SetControlRotation(FRotator(7,0,0));
        }
    }
    if(FParse::Param(FCommandLine::Get(),TEXT("RiftClientTest"))&&!HasAuthority())
    {
        if(auto P=Cast<ARiftCharacter>(GetPawn()))
        {
            if(ProbeTime>6 && ProbeTime<8)P->AddMovementInput(FVector(1,0,0),1.f);
            if(ClientTestStage==0&&ProbeTime>1){SetControlRotation(FRotator(60,0,0));P->ServerFire(GetControlRotation().Vector());ClientTestStage=1;}
            else if(ClientTestStage==1&&ProbeTime>2){UE_LOG(LogTemp,Display,TEXT("RIFT CLIENT TEST %s FIRE ammo=%d"),P->Ammo==29?TEXT("PASS"):TEXT("FAIL"),P->Ammo);P->ServerReload();ClientTestStage=2;}
            else if(ClientTestStage==2&&ProbeTime>4){UE_LOG(LogTemp,Display,TEXT("RIFT CLIENT TEST %s RELOAD ammo=%d"),P->Ammo==30?TEXT("PASS"):TEXT("FAIL"),P->Ammo);P->ServerGrenade(GetControlRotation().Vector());ClientTestStage=3;}
            else if(ClientTestStage==3&&ProbeTime>5){UE_LOG(LogTemp,Display,TEXT("RIFT CLIENT TEST %s GRENADE charges=%d"),P->Grenades==2?TEXT("PASS"):TEXT("FAIL"),P->Grenades);ClientTestStage=4;}
            else if(ClientTestStage==4&&ProbeTime>9)
            {
                const FVector Location=P->GetActorLocation();
                UE_LOG(LogTemp,Display,TEXT("RIFT MOVEMENT DIAG local=%d role=%d mode=%d tick=%d active=%d ignored=%d input=%s velocity=%s base=%s"),P->IsLocallyControlled(),int(P->GetLocalRole()),int(P->GetCharacterMovement()->MovementMode),P->GetCharacterMovement()->IsComponentTickEnabled(),P->GetCharacterMovement()->IsActive(),IsMoveInputIgnored(),*P->GetPendingMovementInputVector().ToString(),*P->GetVelocity().ToString(),*GetNameSafe(P->GetMovementBaseObject()));
                UE_LOG(LogTemp,Display,TEXT("RIFT CLIENT TEST %s MOVEMENT x=%.1f y=%.1f z=%.1f"),Location.X>-1450&&Location.Z>65&&Location.Z<150?TEXT("PASS"):TEXT("FAIL"),Location.X,Location.Y,Location.Z);ClientTestStage=5;
            }
            else if(ClientTestStage==5&&ProbeTime>20)
            {
                int Broken=0;bool Activated=false;for(TActorIterator<ARiftProp> It(GetWorld());It;++It)if(It->bBroken)++Broken;for(TActorIterator<ARiftUFO> It(GetWorld());It;++It)Activated=It->bActivated;
                if(Broken>0&&Activated){UE_LOG(LogTemp,Display,TEXT("RIFT CLIENT TEST PASS REPLICATION broken=%d activated=%d"),Broken,Activated);ClientTestStage=6;}
            }
        }
    }
    const int32 Step=FMath::FloorToInt(ProbeTime/5.f);
    if(Step<=ProbeStep)return;ProbeStep=Step;
    auto GS=GetWorld()->GetGameState<ARiftGameState>();
    auto P=Cast<ARiftCharacter>(GetPawn());
    int Players=0,Enemies=0,Props=0;for(TActorIterator<ARiftCharacter> It(GetWorld());It;++It)++Players;for(TActorIterator<ARiftEnemy> It(GetWorld());It;++It)++Enemies;for(TActorIterator<ARiftProp> It(GetWorld());It;++It)++Props;
    UE_LOG(LogTemp,Display,TEXT("RIFT PROBE net=%d players=%d enemies=%d props=%d phase=%d wave=%d health=%.0f ammo=%d"),int(GetNetMode()),Players,Enemies,Props,GS?GS->Phase:-1,GS?GS->Wave:-1,P?P->Health:-1,P?P->Ammo:-1);
    if(FParse::Param(FCommandLine::Get(),TEXT("RiftAutoTest"))&&HasAuthority()&&P)
    {
        if(Step==4)
        {
            for(TActorIterator<ARiftUFO> It(GetWorld());It;++It){UGameplayStatics::ApplyDamage(*It,250,this,P,nullptr);break;}
            for(TActorIterator<ARiftProp> It(GetWorld());It;++It){UGameplayStatics::ApplyDamage(*It,250,this,P,nullptr);UE_LOG(LogTemp,Display,TEXT("RIFT TEST destructible damage applied"));break;}
        }
        if(Step>5 && Step<18){for(TActorIterator<ARiftEnemy> It(GetWorld());It;++It)UGameplayStatics::ApplyDamage(*It,1000,this,P,nullptr);}
        if(Step==15 && GS && GS->Phase==4)
        {
            P->ServerRestart();int Broken=0;bool Activated=false;for(TActorIterator<ARiftProp> It(GetWorld());It;++It)if(It->bBroken)++Broken;for(TActorIterator<ARiftUFO> It(GetWorld());It;++It)Activated=It->bActivated;
            UE_LOG(LogTemp,Display,TEXT("RIFT HOST TEST %s RESTART phase=%d broken=%d activated=%d"),GS->Phase==0&&Broken==0&&!Activated?TEXT("PASS"):TEXT("FAIL"),GS->Phase,Broken,Activated);
        }
    }
    if(FParse::Param(FCommandLine::Get(),TEXT("RiftCapture"))&&Step==2)ConsoleCommand(TEXT("HighResShot 1"));
}
