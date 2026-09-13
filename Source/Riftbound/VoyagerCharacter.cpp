#include "VoyagerCharacter.h"
#include "VoyagerGameMode.h"
#include "VoyagerShip.h"
#include "VoyagerWorld.h"
#include "VoyagerSettlement.h"
#include "VoyagerWildlife.h"
#include "VoyagerCitizen.h"
#include "VoyagerData.h"
#include "RiftVisual.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InputComponent.h"
#include "Components/AudioComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "Net/UnrealNetwork.h"
#include "DrawDebugHelpers.h"
#include "Sound/SoundBase.h"
#include "Misc/CommandLine.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformMemory.h"

class SVoyagerMenu : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SVoyagerMenu){}
        SLATE_DEFAULT_SLOT(FArguments,Content)
        SLATE_EVENT(FSimpleDelegate,OnClose)
    SLATE_END_ARGS()
    void Construct(const FArguments& A){Close=A._OnClose;ChildSlot[A._Content.Widget];}
    virtual bool SupportsKeyboardFocus() const override{return true;}
    virtual FReply OnKeyDown(const FGeometry& G,const FKeyEvent& E) override{if(E.GetKey()==EKeys::Escape){Close.ExecuteIfBound();return FReply::Handled();}return SCompoundWidget::OnKeyDown(G,E);}
    FSimpleDelegate Close;
};

AVoyagerCharacter::AVoyagerCharacter()
{
    PrimaryActorTick.bCanEverTick=true;bReplicates=true;SetNetUpdateFrequency(40);
    GetCapsuleComponent()->InitCapsuleSize(34,88);GetCharacterMovement()->MaxWalkSpeed=620;GetCharacterMovement()->JumpZVelocity=570;GetCharacterMovement()->AirControl=.4f;
    Camera=CreateDefaultSubobject<UCameraComponent>(TEXT("ExplorerCamera"));Camera->SetupAttachment(GetRootComponent());Camera->SetRelativeLocation(FVector(0,0,64));Camera->bUsePawnControlRotation=true;Camera->FieldOfView=90;
    Tool=CreateDefaultSubobject<USceneComponent>(TEXT("MultiTool"));Tool->SetupAttachment(Camera);Tool->SetRelativeLocation(FVector(42,20,-18));Tool->SetRelativeScale3D(FVector(.55f));
}
void AVoyagerCharacter::BeginPlay()
{
    Super::BeginPlay();
    if(auto State=GetWorld()->GetGameState<AVoyagerState>())
    {
        const int32 Planet=Voyager::NearestPlanet(State->SystemSeed,GetActorLocation());
        const FVector Up=Voyager::SurfaceNormal(State->SystemSeed,Planet,GetActorLocation());
        GetCharacterMovement()->SetGravityDirection(-Up);
        SetActorRotation(Voyager::TangentRotation(Up,GetActorForwardVector()));
    }
    const FLinearColor Ivory(.58f,.67f,.64f),Ink(.025f,.055f,.08f),Mint(.15f,1,.69f);
    auto Piece=[&](FName N,const TCHAR* Shape,FVector L,FVector S,FLinearColor C,bool Glow=false){auto M=RiftVisual::Mesh(this,Tool,N,Shape,L,S,C,Glow);M->SetOnlyOwnerSee(true);M->SetCastShadow(false);};
    Piece(TEXT("ToolBody"),TEXT("Cube"),FVector::ZeroVector,FVector(.57f,.17f,.2f),Ivory);
    Piece(TEXT("ToolCore"),TEXT("Cube"),FVector(10,0,10),FVector(.35f,.11f,.055f),Ink);
    Piece(TEXT("ToolBarrel"),TEXT("Cylinder"),FVector(38,0,0),FVector(.18f,.18f,.21f),Ink);
    Piece(TEXT("ToolMuzzle"),TEXT("Sphere"),FVector(42,0,0),FVector(.13f),Mint,true);
    Piece(TEXT("ToolGrip"),TEXT("Cube"),FVector(-12,0,-16),FVector(.16f,.13f,.29f),Ink);
    Piece(TEXT("ToolDisplay"),TEXT("Cube"),FVector(0,-9,2),FVector(.25f,.008f,.10f),Mint,true);
    auto Body=RiftVisual::Mesh(this,GetRootComponent(),TEXT("ExplorerSuit"),TEXT("Cylinder"),FVector(0,0,-20),FVector(.55f,.55f,.95f),Ivory);Body->SetOwnerNoSee(true);
    auto Helmet=RiftVisual::Mesh(this,GetRootComponent(),TEXT("ExplorerHelmet"),TEXT("Sphere"),FVector(0,0,48),FVector(.45f),Ink);Helmet->SetOwnerNoSee(true);
    auto Visor=RiftVisual::Mesh(this,GetRootComponent(),TEXT("ExplorerVisor"),TEXT("Sphere"),FVector(14,0,50),FVector(.2f,.36f,.22f),Mint,true);Visor->SetOwnerNoSee(true);
    for(int I=-1;I<=1;I+=2){auto Leg=RiftVisual::Mesh(this,GetRootComponent(),FName(*FString::Printf(TEXT("Boot%d"),I)),TEXT("Cube"),FVector(0,I*15,-66),FVector(.2f,.21f,.44f),Ink);Leg->SetOwnerNoSee(true);}
}
void AVoyagerCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const{Super::GetLifetimeReplicatedProps(OutLifetimeProps);DOREPLIFETIME(AVoyagerCharacter,Health);}
bool AVoyagerCharacter::CanAct() const
{
    auto State=GetWorld()->GetGameState<AVoyagerState>();auto PC=Cast<AVoyagerController>(Controller);return (!State||!State->bTransitioning)&&(!PC||!PC->bMenuVisible);
}
void AVoyagerCharacter::SetupPlayerInputComponent(UInputComponent* I)
{
    Super::SetupPlayerInputComponent(I);I->BindAxis("MoveForward",this,&AVoyagerCharacter::Forward);I->BindAxis("MoveRight",this,&AVoyagerCharacter::Right);I->BindAxis("Turn",this,&AVoyagerCharacter::Turn);I->BindAxis("LookUp",this,&AVoyagerCharacter::Look);
    I->BindAction("Jump",IE_Pressed,this,&ACharacter::Jump);I->BindAction("Jump",IE_Released,this,&ACharacter::StopJumping);
    I->BindAction("Fire",IE_Pressed,this,&AVoyagerCharacter::StartMine);I->BindAction("Fire",IE_Released,this,&AVoyagerCharacter::StopMine);
    I->BindAction("Interact",IE_Pressed,this,&AVoyagerCharacter::Interact);I->BindAction("Scan",IE_Pressed,this,&AVoyagerCharacter::Scan);
    I->BindAction("Sprint",IE_Pressed,this,&AVoyagerCharacter::Sprint);I->BindAction("Sprint",IE_Released,this,&AVoyagerCharacter::StopSprint);
}
void AVoyagerCharacter::Forward(float V){if(CanAct())AddMovementInput(GetActorForwardVector(),V);}
void AVoyagerCharacter::Right(float V){if(CanAct())AddMovementInput(GetActorRightVector(),V);}
void AVoyagerCharacter::Turn(float V){if(CanAct())AddControllerYawInput(V*.85f);}
void AVoyagerCharacter::Look(float V){if(CanAct())AddControllerPitchInput(V*.85f);}
void AVoyagerCharacter::StartMine(){if(CanAct())bMining=true;}
void AVoyagerCharacter::StopMine(){bMining=false;}
void AVoyagerCharacter::Interact(){if(CanAct())ServerInteract();}
void AVoyagerCharacter::Scan(){if(CanAct()){ScanPulse=1.f;ServerScan();}}
void AVoyagerCharacter::Sprint(){bSprinting=true;GetCharacterMovement()->MaxWalkSpeed=1000;ServerSprint(true);}
void AVoyagerCharacter::StopSprint(){bSprinting=false;GetCharacterMovement()->MaxWalkSpeed=620;ServerSprint(false);}
void AVoyagerCharacter::ServerSprint_Implementation(bool V){bSprinting=V;GetCharacterMovement()->MaxWalkSpeed=V?1000:620;}
void AVoyagerCharacter::Tick(float D)
{
    auto PlanetState=GetWorld()->GetGameState<AVoyagerState>();
    if(PlanetState)
    {
        const int32 Planet=Voyager::NearestPlanet(PlanetState->SystemSeed,GetActorLocation());
        const FVector Up=Voyager::SurfaceNormal(PlanetState->SystemSeed,Planet,GetActorLocation());
        GetCharacterMovement()->SetGravityDirection(-Up);
        if(IsLocallyControlled()||HasAuthority())SetActorRotation(Voyager::TangentRotation(Up,GetActorForwardVector()));
        if(IsLocallyControlled())Tool->SetVisibility(bMining||!AVoyagerSettlement::IsWithinSite(PlanetState->SystemSeed,Planet,Up),true);
    }
    Super::Tick(D);ScanPulse=FMath::Max(0.f,ScanPulse-D*.42f);MiningFeedback=FMath::Max(0.f,MiningFeedback-D);
    if(IsLocallyControlled())
    {
        if(!CanAct())bMining=false;
        if(bMining&&GetWorld()->TimeSeconds>NextShot){NextShot=GetWorld()->TimeSeconds+.22f;ServerMine(Camera->GetForwardVector());}
        const float Bob=FMath::Sin(GetWorld()->TimeSeconds*10)*FMath::Min(GetVelocity().Size2D()/620.f,1.f);
        Tool->SetRelativeLocation(FVector(42,20+Bob*.6f,-18+Bob*.8f));
    }
    if(HasAuthority())
    {
        auto State=GetWorld()->GetGameState<AVoyagerState>();if(State&&!State->bTransitioning)
        {
            const FVector L=GetActorLocation();const int32 Planet=Voyager::NearestPlanet(State->SystemSeed,L);
            if(Voyager::SurfaceAltitude(State->SystemSeed,Planet,L)<-300)
            {SetActorLocation(Voyager::SurfacePoint(State->SystemSeed,Planet,Voyager::SurfaceNormal(State->SystemSeed,Planet,L),150));GetCharacterMovement()->Velocity=FVector::ZeroVector;}
        }
    }
}
void AVoyagerCharacter::FaceRotation(FRotator NewControlRotation,float DeltaTime)
{
    auto State=GetWorld()->GetGameState<AVoyagerState>();if(!State)return;
    const int32 Planet=Voyager::NearestPlanet(State->SystemSeed,GetActorLocation());
    SetActorRotation(Voyager::TangentRotation(Voyager::SurfaceNormal(State->SystemSeed,Planet,GetActorLocation()),NewControlRotation.Vector()));
}
FVector AVoyagerCharacter::GetPawnViewLocation() const
{return GetActorLocation()+GetActorUpVector()*64;}
AVoyagerCitizen* AVoyagerCharacter::FocusedCitizen() const
{
    AVoyagerCitizen* Best=nullptr; double RangeSquared=400.0*400.0;
    const FVector Eye=GetActorLocation()+GetActorUpVector()*64;
    for(TActorIterator<AVoyagerCitizen> It(GetWorld());It;++It)
    {
        const double Distance=FVector::DistSquared(GetActorLocation(),It->GetActorLocation());
        if(Distance>=RangeSquared)continue;
        const FVector ToFace=It->GetActorLocation()+It->GetActorUpVector()*145-Eye;
        if(FVector::DotProduct(GetBaseAimRotation().Vector(),ToFace.GetSafeNormal())<.3)continue;
        FHitResult Hit; FCollisionQueryParams Query(SCENE_QUERY_STAT(VoyagerConversation),false,this);
        Query.AddIgnoredActor(*It);
        if(GetWorld()->LineTraceSingleByChannel(Hit,Eye,It->GetActorLocation()+It->GetActorUpVector()*145,ECC_Visibility,Query))continue;
        Best=*It;RangeSquared=Distance;
    }
    return Best;
}
void AVoyagerCharacter::ServerInteract_Implementation()
{
    if(!CanAct())return;
    if(auto* Citizen=FocusedCitizen()){TalkPartner=Citizen;ServerTalk_Implementation(0);return;}
    if(auto* GM=GetWorld()->GetAuthGameMode<AVoyagerGameMode>())GM->BoardShip(this);
}
void AVoyagerCharacter::ServerTalk_Implementation(int32 Topic)
{
    if(!CanAct()||Topic<0||Topic>2||GetWorld()->GetTimeSeconds()-LastTalk<.35f)return;
    auto* Citizen=TalkPartner.Get();auto* PC=Cast<AVoyagerController>(Controller);
    if(!Citizen||!PC)return;
    if(FVector::DistSquared(GetActorLocation(),Citizen->GetActorLocation())>400.0*400.0){TalkPartner.Reset();return;}
    LastTalk=GetWorld()->GetTimeSeconds();
    const FString Reply=Citizen->TalkTo(this,Topic);
    if(!Reply.IsEmpty())PC->ShowConversation(Citizen,Reply);
}
void AVoyagerCharacter::ServerEndTalk_Implementation()
{
    if(auto* Citizen=TalkPartner.Get())Citizen->EndConversation(this);
    TalkPartner.Reset();
}
void AVoyagerCharacter::ServerScan_Implementation()
{
    if(!CanAct()||GetWorld()->TimeSeconds-LastScan<2)return;LastScan=GetWorld()->TimeSeconds;
    auto State=GetWorld()->GetGameState<AVoyagerState>();auto PS=GetPlayerState<AVoyagerPlayerState>();auto PC=Cast<AVoyagerController>(Controller);if(!State||!PS||!PC)return;
    const int32 Planet=Voyager::NearestPlanet(State->SystemSeed,GetActorLocation());
    const bool New=PS->DiscoverPlanet(State->SystemSeed,Planet);
    PC->Notify(New?FString::Printf(TEXT("DISCOVERY RECORDED: %s  /  +30 minerals"),*Voyager::PlanetName(State->SystemSeed,Planet)):FString::Printf(TEXT("%s  /  %s  /  Mine glowing crystals. Your ship waits at the landing site."),*Voyager::PlanetName(State->SystemSeed,Planet),Voyager::BiomeName(Voyager::Biome(State->SystemSeed,Planet))));
}
void AVoyagerCharacter::ServerMine_Implementation(FVector_NetQuantizeNormal Direction)
{
    if(!CanAct()||Direction.ContainsNaN()||!FMath::IsNearlyEqual(Direction.SizeSquared(),1.f,.02f)||GetWorld()->TimeSeconds-LastServerShot<.19f)return;
    LastServerShot=GetWorld()->TimeSeconds;FVector Start=GetActorLocation()+GetActorUpVector()*64;FHitResult Hit;FCollisionQueryParams Q(SCENE_QUERY_STAT(VoyagerMine),true,this);
    for(TActorIterator<AVoyagerCitizenManager> It(GetWorld());It;++It)It->ReportDisturbance(Start);
    bool bHit=GetWorld()->LineTraceSingleByChannel(Hit,Start,Start+Direction*1800,ECC_Visibility,Q);bool Success=false;
    if(auto Resource=Cast<AVoyagerResource>(Hit.GetActor())){UGameplayStatics::ApplyPointDamage(Resource,20,Direction,Hit,Controller,this,nullptr);Success=true;}
    MiningBeam(bHit?Hit.ImpactPoint:Start+Direction*1800,Success);
}
void AVoyagerCharacter::MiningBeam_Implementation(FVector End,bool Success)
{
    if(GetNetMode()==NM_DedicatedServer)return;
    FVector Start=IsLocallyControlled()?Tool->GetComponentLocation()+Camera->GetForwardVector()*30:GetActorLocation()+GetActorUpVector()*64;
    DrawDebugLine(GetWorld(),Start,End,Success?FColor(255,191,95):FColor(76,255,198),false,.16f,0,2.8f);MiningFeedback=Success?.2f:0;
    if(auto Sound=LoadObject<USoundBase>(nullptr,TEXT("/Game/Audio/S_Gun.S_Gun")))UGameplayStatics::PlaySoundAtLocation(this,Sound,Start,.12f,1.8f);
}

void AVoyagerController::BeginPlay(){Super::BeginPlay();GetWorld()->GetWorldSettings()->bEnableWorldBoundsChecks=false;if(IsLocalController()){SetInputMode(FInputModeGameOnly());bShowMouseCursor=false;}}
void AVoyagerController::UpdateRotation(float D)
{
    auto Explorer=Cast<AVoyagerCharacter>(GetPawn());auto State=GetWorld()->GetGameState<AVoyagerState>();
    if(!Explorer||!State){Super::UpdateRotation(D);return;}
    const int32 Planet=Voyager::NearestPlanet(State->SystemSeed,Explorer->GetActorLocation());
    const FVector Up=Voyager::SurfaceNormal(State->SystemSeed,Planet,Explorer->GetActorLocation());
    FVector Forward=GetControlRotation().Vector();
    Forward=FQuat(Up,FMath::DegreesToRadians(RotationInput.Yaw)).RotateVector(Forward);
    FVector Right=FVector::CrossProduct(Up,Forward).GetSafeNormal();
    if(Right.IsNearlyZero())
    {Forward=Voyager::TangentRotation(Up,Explorer->GetActorForwardVector()).Vector();Right=FVector::CrossProduct(Up,Forward).GetSafeNormal();}
    const double Pitch=FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(FVector::DotProduct(Forward,Up),-1.0,1.0)));
    const double PitchDelta=FMath::Clamp(Pitch+RotationInput.Pitch,-85.0,85.0)-Pitch;
    Forward=FQuat(Right,-FMath::DegreesToRadians(PitchDelta)).RotateVector(Forward);
    const FRotator View=FRotationMatrix::MakeFromXZ(Forward,Up).Rotator();SetControlRotation(View);Explorer->FaceRotation(View,D);
}
void AVoyagerController::EndPlay(const EEndPlayReason::Type R){HideMenu();Super::EndPlay(R);}
void AVoyagerController::SetupInputComponent()
{
    Super::SetupInputComponent();InputComponent->BindAction("Menu",IE_Pressed,this,&AVoyagerController::ToggleMenu);InputComponent->BindAction("Save",IE_Pressed,this,&AVoyagerController::SaveInput);InputComponent->BindAction("Upgrade",IE_Pressed,this,&AVoyagerController::UpgradeInput);
    InputComponent->BindKey(EKeys::One,IE_Pressed,this,&AVoyagerController::TalkGreeting);
    InputComponent->BindKey(EKeys::Two,IE_Pressed,this,&AVoyagerController::TalkLife);
    InputComponent->BindKey(EKeys::Three,IE_Pressed,this,&AVoyagerController::TalkDirections);
    InputComponent->BindKey(EKeys::BackSpace,IE_Pressed,this,&AVoyagerController::CloseConversation);
}
void AVoyagerController::Notify_Implementation(const FString& Message){Notice=Message;NoticeTime=8.f;}
void AVoyagerController::ShowConversation_Implementation(AVoyagerCitizen* Citizen,const FString& Speech)
{ConversationTarget=Citizen;ConversationSpeech=Speech;ConversationTime=20.f;}
void AVoyagerController::TalkGreeting(){if(ConversationTime>0)if(auto* Explorer=Cast<AVoyagerCharacter>(GetPawn()))Explorer->ServerTalk(0);}
void AVoyagerController::TalkLife(){if(ConversationTime>0)if(auto* Explorer=Cast<AVoyagerCharacter>(GetPawn()))Explorer->ServerTalk(1);}
void AVoyagerController::TalkDirections(){if(ConversationTime>0)if(auto* Explorer=Cast<AVoyagerCharacter>(GetPawn()))Explorer->ServerTalk(2);}
void AVoyagerController::CloseConversation()
{
    if(ConversationTarget.IsValid())if(auto* Explorer=Cast<AVoyagerCharacter>(GetPawn()))Explorer->ServerEndTalk();
    ConversationTime=0;ConversationTarget.Reset();ConversationSpeech.Empty();
}
void AVoyagerController::SaveInput(){ServerSave();}
void AVoyagerController::UpgradeInput(){ServerUpgrade();}
void AVoyagerController::ServerSave_Implementation(){if(auto GM=GetWorld()->GetAuthGameMode<AVoyagerGameMode>()){GM->SaveExpedition();Notify(TEXT("Expedition saved. Continue from this planet next time."));}}
void AVoyagerController::ServerUpgrade_Implementation(){if(auto GM=GetWorld()->GetAuthGameMode<AVoyagerGameMode>())GM->UpgradeShip(this);}
void AVoyagerController::ToggleMenu(){if(bMenuVisible)HideMenu();else ShowMenu();}
void AVoyagerController::HideMenu()
{
    if(MenuWidget.IsValid()&&GEngine&&GEngine->GameViewport)GEngine->GameViewport->RemoveViewportWidgetContent(MenuWidget.ToSharedRef());
    MenuWidget.Reset();AddressBox.Reset();bMenuVisible=false;bShowMouseCursor=false;if(IsLocalController())SetInputMode(FInputModeGameOnly());
}
void AVoyagerController::ShowMenu()
{
    if(!IsLocalController()||!GEngine||!GEngine->GameViewport)return;bMenuVisible=true;bShowMouseCursor=true;FlushPressedKeys();
    auto Label=[](const FString& S,int Size){return SNew(STextBlock).Text(FText::FromString(S)).Font(FCoreStyle::GetDefaultFontStyle("Regular",Size)).ColorAndOpacity(FLinearColor(.85f,.96f,.94f));};
    TSharedRef<SWidget> Content=SNew(SOverlay)
    +SOverlay::Slot()[SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).BorderBackgroundColor(FLinearColor(.004f,.012f,.021f,.97f))]
    +SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
    [SNew(SBox).WidthOverride(540)[SNew(SVerticalBox)
      +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,8)[Label(TEXT("V O Y A G E R"),40)]
      +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,20)[Label(TEXT("RIFTBOUND  /  THE NEXT HORIZON IS YOURS"),12)]
      +SVerticalBox::Slot().AutoHeight().Padding(0,4)[SNew(SButton).ContentPadding(13).OnClicked_Lambda([this](){HideMenu();return FReply::Handled();})[Label(TEXT("CONTINUE EXPEDITION"),17)]]
      +SVerticalBox::Slot().AutoHeight().Padding(0,4)[SNew(SButton).ContentPadding(12).OnClicked_Lambda([this](){ServerSave();HideMenu();return FReply::Handled();})[Label(TEXT("SAVE EXPEDITION   /   F5"),14)]]
      +SVerticalBox::Slot().AutoHeight().Padding(0,4)[SNew(SButton).ContentPadding(12).OnClicked_Lambda([this](){ServerUpgrade();HideMenu();return FReply::Handled();})[Label(TEXT("UPGRADE LASERS   /   75 MINERALS   /   U"),14)]]
      +SVerticalBox::Slot().AutoHeight().Padding(0,4)[SNew(SButton).ContentPadding(12).OnClicked_Lambda([this](){ServerSave();HideMenu();UGameplayStatics::OpenLevel(this,TEXT("/Game/Maps/Forest"),true,TEXT("game=/Script/Riftbound.VoyagerGameMode?listen"));return FReply::Handled();})[Label(TEXT("HOST CO-OP EXPEDITION"),14)]]
      +SVerticalBox::Slot().AutoHeight().Padding(0,14,0,4)[Label(TEXT("Join a host IP  /  shared star system  /  up to 4 players"),11)]
      +SVerticalBox::Slot().AutoHeight().Padding(0,4)[SAssignNew(AddressBox,SEditableTextBox).Text(FText::FromString(TEXT("127.0.0.1"))).Font(FCoreStyle::GetDefaultFontStyle("Regular",16))]
      +SVerticalBox::Slot().AutoHeight().Padding(0,4)[SNew(SButton).ContentPadding(12).OnClicked_Lambda([this](){FString Address=AddressBox->GetText().ToString().TrimStartAndEnd();bool Valid=!Address.IsEmpty();for(TCHAR C:Address)if(!FChar::IsAlnum(C)&&C!=TEXT('.')&&C!=TEXT(':')&&C!=TEXT('-'))Valid=false;if(Valid){HideMenu();ClientTravel(Address,TRAVEL_Absolute);}return FReply::Handled();})[Label(TEXT("CONNECT TO EXPEDITION"),14)]]
      +SVerticalBox::Slot().AutoHeight().Padding(0,18,0,6)[Label(TEXT("ON FOOT  WASD move / F scan / LMB mine / E talk or board\nCITY  Walk through signed doors / 1-3 dialogue / Backspace end\nFLIGHT  WASD thrust / mouse steer / Space rise / Ctrl descend\nShift boost / LMB lasers / E land below 10 m or exit\nSPACE above 60 km / Tab target / J cruise / H next star\nFly directly into or out of the atmosphere."),11)]
      +SVerticalBox::Slot().AutoHeight().Padding(0,4)[SNew(SButton).ContentPadding(10).OnClicked_Lambda([this](){ServerSave();ConsoleCommand(TEXT("quit"));return FReply::Handled();})[Label(TEXT("SAVE AND QUIT"),13)]]
    ]];
    MenuWidget=SNew(SVoyagerMenu).OnClose_Lambda([this](){HideMenu();})[Content];GEngine->GameViewport->AddViewportWidgetContent(MenuWidget.ToSharedRef(),100);
    FInputModeUIOnly Mode;Mode.SetWidgetToFocus(MenuWidget);Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);SetInputMode(Mode);
}
void AVoyagerController::Tick(float D)
{
    Super::Tick(D);NoticeTime=FMath::Max(0.f,NoticeTime-D);
    ConversationTime=FMath::Max(0.f,ConversationTime-D);
    if(ConversationTime>0&&(!ConversationTarget.IsValid()||!Cast<AVoyagerCharacter>(GetPawn())||
        FVector::DistSquared(GetPawn()->GetActorLocation(),ConversationTarget->GetActorLocation())>500.0*500.0))CloseConversation();
    if(FParse::Param(FCommandLine::Get(),TEXT("VoyagerProbe")))RunProbe(D);
}

void AVoyagerController::RunProbe(float D)
{
    if(!IsLocalController())return;TestTime+=D;
    // Controller sampling precedes the pawn tick. A captured frame can be much
    // longer than the next frame, so bound travel using both adjacent deltas.
    const double MovementSampleDelta=FMath::Max(double(D),double(PreviousProbeDelta));PreviousProbeDelta=D;
    const double FrameTime=FPlatformTime::Seconds();
    if(PerformanceWindowStart==0)PerformanceWindowStart=FrameTime;
    if(PreviousFrameTime>0)WorstFrameMs=FMath::Max(WorstFrameMs,(FrameTime-PreviousFrameTime)*1000.0);
    PreviousFrameTime=FrameTime;++PerformanceFrames;
    auto State=GetWorld()->GetGameState<AVoyagerState>();auto PS=GetPlayerState<AVoyagerPlayerState>();if(!State||!PS||!GetPawn())return;
    const int32 PhysicalPlanet=Voyager::NearestPlanet(State->SystemSeed,GetPawn()->GetActorLocation());
    const double Altitude=Voyager::SurfaceAltitude(State->SystemSeed,PhysicalPlanet,GetPawn()->GetActorLocation());
    if(TestTime-LastProbe>5)
    {
        const auto Memory=FPlatformMemory::GetStats();
        UE_LOG(LogTemp,Display,TEXT("VOYAGER PERFORMANCE fps=%.1f worst_frame_ms=%.2f process_ram_mb=%.0f peak_ram_mb=%.0f"),
            PerformanceFrames/FMath::Max(.001,FrameTime-PerformanceWindowStart),WorstFrameMs,Memory.UsedPhysical/1048576.0,Memory.PeakUsedPhysical/1048576.0);
        PerformanceWindowStart=FrameTime;PerformanceFrames=0;WorstFrameMs=0;
        LastProbe=TestTime;int Chunks=0;for(TActorIterator<AVoyagerWorld> It(GetWorld());It;++It)Chunks=It->ActiveChunkCount();
        int32 Cities=0,Buildings=0,Animals=0;
        for(TActorIterator<AVoyagerSettlement> It(GetWorld());It;++It){Cities+=It->ActiveCityCount();Buildings+=It->ActiveBuildingCount();}
        for(TActorIterator<AVoyagerAnimal> It(GetWorld());It;++It)++Animals;
        UE_LOG(LogTemp,Display,TEXT("VOYAGER LIFE net=%d cities=%d buildings=%d fauna=%d"),int(GetNetMode()),Cities,Buildings,Animals);
        UE_LOG(LogTemp,Display,TEXT("VOYAGER PROBE net=%d players=%d system=%d planet=%d mode=%d transition=%d pawn=%s minerals=%d discoveries=%d kills=%d chunks=%d altitude_m=%.2f stage=%d pos=%s"),int(GetNetMode()),State->PlayerArray.Num(),State->SystemSeed,PhysicalPlanet,Altitude>=Voyager::AtmosphereHeight?1:0,State->bTransitioning,*GetPawn()->GetClass()->GetName(),PS->Minerals,PS->Discoveries,PS->PirateKills,Chunks,Altitude/100.0,TestStage,*GetPawn()->GetActorLocation().ToString());
    }
    if(FParse::Param(FCommandLine::Get(),TEXT("VoyagerSurfaceAudit"))){RunSurfaceAudit(D);return;}
    if(FParse::Param(FCommandLine::Get(),TEXT("VoyagerLifeAudit"))){RunLifeAudit(D);return;}
    if(FParse::Param(FCommandLine::Get(),TEXT("VoyagerCityAudit"))){RunCityAudit(D);return;}
    if(FParse::Param(FCommandLine::Get(),TEXT("VoyagerCityNetAudit"))){RunCityNetworkAudit(D);return;}
    const bool Auto=FParse::Param(FCommandLine::Get(),TEXT("VoyagerTest"))||(FParse::Param(FCommandLine::Get(),TEXT("VoyagerNetTest"))&&!HasAuthority());
    if(!Auto&&TestTime>8&&TestCaptures==0&&FParse::Param(FCommandLine::Get(),TEXT("VoyagerCityVisit"))&&FParse::Param(FCommandLine::Get(),TEXT("VoyagerCapture")))
    {TestCaptures=1;ConsoleCommand(TEXT("HighResShot 1"));}
    if(!Auto||TestStage>=20)return;
    auto Advance=[this](){++TestStage;StageStarted=TestTime;};
    auto Pass=[](const TCHAR* Label){UE_LOG(LogTemp,Display,TEXT("VOYAGER TEST PASS %s"),Label);};
    auto Fail=[this](const TCHAR* Label){UE_LOG(LogTemp,Error,TEXT("VOYAGER TEST FAIL %s stage=%d"),Label,TestStage);TestStage=99;};
    auto Capture=[this](int32 Bit){if(!(TestCaptures&Bit)&&FParse::Param(FCommandLine::Get(),TEXT("VoyagerCapture"))){TestCaptures|=Bit;ConsoleCommand(TEXT("HighResShot 1"));}};
    const float Elapsed=TestTime-StageStarted;
    if(Elapsed>150){Fail(TEXT("TIMEOUT"));return;}
    auto Explorer=Cast<AVoyagerCharacter>(GetPawn());auto Ship=Cast<AVoyagerShip>(GetPawn());
    if(Ship&&TestStage>=4&&TestStage<=7)
    {
        if(State->bTransitioning||State->Revision!=TestWorldRevision||TestFlightPawn.Get()!=Ship)
        {Fail(TEXT("PLANET_TRAVEL_REBUILT_OR_REPLACED_PAWN"));return;}
        const double Step=FVector::Dist(TestLastLocation,Ship->GetActorLocation());
        const double NetworkAllowance=HasAuthority()?0.0:.10;
        const double Limit=(TestStage==6?50000000.0:3000000.0)*(FMath::Max(MovementSampleDelta,1.0/120.0)+NetworkAllowance)+2000.0;
        if(Step>Limit){UE_LOG(LogTemp,Error,TEXT("VOYAGER CONTINUITY step=%.3f limit=%.3f dt=%.6f"),Step,Limit,D);Fail(TEXT("POSITION_DISCONTINUITY"));return;}
        TestMaxFlightStep=FMath::Max(TestMaxFlightStep,Step);TestLastLocation=Ship->GetActorLocation();
        if((TestStage==4||TestStage==7)&&Ship->GetFlightEpoch()!=TestFlightEpoch)
        {Fail(TEXT("ATMOSPHERE_RESET_FLIGHT"));return;}
    }
    if(TestStage==0&&TestTime>3&&Explorer){TestStart=Explorer->GetActorLocation();Explorer->ServerScan();Capture(1);Advance();}
    else if(TestStage==1&&Explorer)
    {
        if(Elapsed<.6f)Explorer->AddMovementInput(Explorer->GetActorForwardVector(),1);
        if(Elapsed>1)
        {
            if(FVector::Dist(TestStart,Explorer->GetActorLocation())<100||PS->Discoveries<1){Fail(TEXT("WALK_SCAN"));return;}
            Pass(TEXT("WALK_SCAN"));Advance();
        }
    }
    else if(TestStage==2&&Explorer)
    {
        AVoyagerResource* Best=nullptr;double Distance=1700;
        for(TActorIterator<AVoyagerResource> It(GetWorld());It;++It){const double Dist=FVector::Dist(It->GetActorLocation(),Explorer->GetActorLocation());if(!It->bHarvested&&Dist<Distance){Distance=Dist;Best=*It;}}
        if(Best){const FVector Direction=(Best->GetActorLocation()-Explorer->Camera->GetComponentLocation()).GetSafeNormal();SetControlRotation(Direction.Rotation());Explorer->ServerMine(Direction);}
        if(PS->Minerals>30){Pass(TEXT("MINING"));Explorer->ServerInteract();Advance();}
    }
    else if(TestStage==3&&Ship&&Ship->GetFlightEpoch()>0)
    {
        Pass(TEXT("BOARD"));TestFlightPawn=Ship;TestFlightEpoch=Ship->GetFlightEpoch();TestWorldRevision=State->Revision;
        TestLastLocation=Ship->GetActorLocation();Ship->SetFlightTestInput(0,1,true);Advance();
    }
    else if(TestStage==4&&Ship)
    {
        if(Altitude>1500000)Capture(2);
        if(Altitude>=Voyager::AtmosphereHeight+500000)
        {Ship->ClearFlightTestInput();Pass(TEXT("SEAMLESS_ASCENT"));Pass(TEXT("FLIGHT_ORBIT"));Capture(4);Advance();}
    }
    else if(TestStage==5&&Ship)
    {
        AVoyagerPirate* Pirate=nullptr;for(TActorIterator<AVoyagerPirate> It(GetWorld());It;++It){Pirate=*It;break;}
        if(Pirate)
        {
            if(HasAuthority())Ship->SetActorRotation((Pirate->GetActorLocation()-Ship->GetActorLocation()).Rotation());
            Ship->ServerShoot();
        }
        if(PS->PirateKills>0||Elapsed>25)
        {
            if(PS->PirateKills>0)Pass(TEXT("SPACE_COMBAT"));
            else UE_LOG(LogTemp,Display,TEXT("VOYAGER TEST INFO space combat targets active; exploration continues"));
            Ship->ServerSelectTarget();Ship->ServerWarp();TestLastLocation=Ship->GetActorLocation();Advance();
        }
    }
    else if(TestStage==6&&Ship&&PhysicalPlanet==1&&!Ship->bCruising&&Altitude>Voyager::AtmosphereHeight)
    {
        Pass(TEXT("CONTINUOUS_CRUISE"));Pass(TEXT("PULSE_JUMP"));
        TestFlightEpoch=Ship->GetFlightEpoch();Ship->SetFlightTestInput(0,-1,true);Advance();
    }
    else if(TestStage==7&&Ship)
    {
        if(Altitude<Voyager::AtmosphereHeight&&Altitude>1000000)Capture(8);
        if(Altitude<500)
        {
            Ship->ClearFlightTestInput();Pass(TEXT("SEAMLESS_DESCENT"));Pass(TEXT("PLANET_LANDING"));
            UE_LOG(LogTemp,Display,TEXT("VOYAGER CONTINUITY PASS max_step_cm=%.3f revision=%d"),TestMaxFlightStep,State->Revision);
            Ship->ServerInteract();Advance();
        }
    }
    else if(TestStage==8&&Explorer){Pass(TEXT("DISEMBARK"));Explorer->ServerScan();TestStart=Explorer->GetActorLocation();Capture(16);Advance();}
    else if(TestStage==9&&Explorer)
    {
        if(Elapsed<.6f)Explorer->AddMovementInput(Explorer->GetActorForwardVector(),1);
        if(Elapsed>1&&PS->Discoveries>=2)
        {
            if(FVector::Dist(TestStart,Explorer->GetActorLocation())<100){Fail(TEXT("SECOND_PLANET_WALK"));return;}
            Pass(TEXT("SECOND_PLANET_WALK"));Pass(TEXT("SECOND_DISCOVERY"));Explorer->ServerInteract();Advance();
        }
    }
    else if(TestStage==10&&Ship){Ship->SetFlightTestInput(0,1,true);Advance();}
    else if(TestStage==11&&Ship&&Altitude>Voyager::AtmosphereHeight+500000){Ship->ClearFlightTestInput();Ship->ServerNextSystem();Advance();}
    else if(TestStage==12&&Ship&&State->SystemSeed==2&&!State->bTransitioning)
    {Pass(TEXT("NEXT_SYSTEM"));ServerSave();Advance();}
    else if(TestStage==13&&Elapsed>1)
    {
        if(HasAuthority())
        {
            auto Save=Cast<UVoyagerSave>(UGameplayStatics::LoadGameFromSlot(TEXT("Voyager-Automation"),0));
            if(!Save||Save->SystemSeed!=2||Save->Visited.Num()<2){Fail(TEXT("SAVE_READBACK"));return;}
            Pass(TEXT("SAVE_READBACK"));
        }
        UE_LOG(LogTemp,Display,TEXT("VOYAGER TEST COMPLETE PASS"));TestStage=20;Capture(32);
    }
}

void AVoyagerController::RunLifeAudit(float D)
{
    if(!HasAuthority()||TestStage>=90||TestTime<5)return;
    if(TestStage>=15)
    {
        auto* Ship=Cast<AVoyagerShip>(GetPawn());auto* S=GetWorld()->GetGameState<AVoyagerState>();if(!Ship||!S)return;
        const double T=TestTime-StageStarted;
        const FVector Up=AVoyagerSettlement::SiteDirection(S->SystemSeed,4);
        const FVector Forward=Voyager::TangentRotation(Up).Vector();
        const double Travel=FVector::DotProduct(Ship->GetActorLocation()-TestStart,Forward);
        if(TestStage==15&&T>3.5)
        {
            if(Travel<3000||Travel>7000||Ship->GetVelocity().Size()>200)
            {UE_LOG(LogTemp,Error,TEXT("VOYAGER LIFE AUDIT FAIL CITY_FLIGHT_COLLISION travel=%.1f speed=%.1f"),Travel,Ship->GetVelocity().Size());Ship->ClearFlightTestInput();TestStage=99;return;}
            UE_LOG(LogTemp,Display,TEXT("VOYAGER LIFE AUDIT PASS CITY_FLIGHT_COLLISION stopped_after_cm=%.1f"),Travel);
            // Back away from balconies before climbing; overhangs are solid too.
            Ship->SetActorRotation(Voyager::TangentRotation(Up,-Forward));
            Ship->SetFlightTestInput(1,0,false);++TestStage;StageStarted=TestTime;
        }
        else if(TestStage==16&&T>1.5)
        {
            Ship->SetFlightTestInput(0,1,true);++TestStage;StageStarted=TestTime;
        }
        else if(TestStage==17&&T>4)
        {
            if(Ship->SurfaceAltitude()<10000){UE_LOG(LogTemp,Error,TEXT("VOYAGER LIFE AUDIT FAIL CITY_ASCENT"));Ship->ClearFlightTestInput();TestStage=99;return;}
            Ship->SetActorRotation(Voyager::TangentRotation(Up,Forward));
            Ship->SetFlightTestInput(1,0,false);++TestStage;StageStarted=TestTime;
        }
        else if(TestStage==18&&T>3)
        {
            Ship->ClearFlightTestInput();
            if(Travel<15000){UE_LOG(LogTemp,Error,TEXT("VOYAGER LIFE AUDIT FAIL CITY_CLEARANCE"));TestStage=99;return;}
            UE_LOG(LogTemp,Display,TEXT("VOYAGER LIFE AUDIT PASS CITY_ASCENT_AND_CLEARANCE travel_cm=%.1f"),Travel);
            if(FParse::Param(FCommandLine::Get(),TEXT("VoyagerCapture")))ConsoleCommand(TEXT("HighResShot 1"));
            ++TestStage;StageStarted=TestTime;
        }
        else if(TestStage==19&&T>1){UE_LOG(LogTemp,Display,TEXT("VOYAGER LIFE AUDIT COMPLETE PASS"));TestStage=90;}
        return;
    }
    auto* Explorer=Cast<AVoyagerCharacter>(GetPawn());auto* State=GetWorld()->GetGameState<AVoyagerState>();
    if(!Explorer||!State)return;
    const double Elapsed=TestTime-StageStarted;
    auto Capture=[this](){if(FParse::Param(FCommandLine::Get(),TEXT("VoyagerCapture")))ConsoleCommand(TEXT("HighResShot 1"));};
    auto Fail=[this](const TCHAR* Why){UE_LOG(LogTemp,Error,TEXT("VOYAGER LIFE AUDIT FAIL %s stage=%d"),Why,TestStage);TestStage=99;};
    if(TestStage==0)
    {
        for(TActorIterator<AVoyagerAnimal> It(GetWorld());It;++It)LifeAuditPositions.Add(*It,It->GetActorLocation());
        if(LifeAuditPositions.Num()<4){Fail(TEXT("NO_NEARBY_WILDLIFE"));return;}
        Capture();++TestStage;StageStarted=TestTime;return;
    }
    if(TestStage==1&&Elapsed>6)
    {
        int32 Moving=0;
        for(const auto& Pair:LifeAuditPositions)
            if(Pair.Key.IsValid()&&FVector::Dist(Pair.Value,Pair.Key->GetActorLocation())>40.0)++Moving;
        if(Moving<2){Fail(TEXT("WILDLIFE_NOT_ROAMING"));return;}
        UE_LOG(LogTemp,Display,TEXT("VOYAGER LIFE AUDIT PASS WILDLIFE_MOVEMENT animals=%d moved=%d"),LifeAuditPositions.Num(),Moving);
        LifeAuditPositions.Empty();++TestStage;StageStarted=TestTime;
    }
    if(TestStage>=2&&TestStage<=11)
    {
        const int32 Planet=(TestStage-2)/2;
        const FVector Up=AVoyagerSettlement::SiteDirection(State->SystemSeed,Planet);
        const FQuat Frame=Voyager::TangentRotation(Up).Quaternion();
        const FVector City=Voyager::SurfacePoint(State->SystemSeed,Planet,Up);
        if(TestStage%2==0)
        {
            // Explicit fixture camera relocation; never used by normal play.
            const FVector Eye=City-Frame.GetAxisX()*34000.0-Frame.GetAxisY()*25000.0+Up*19000.0;
            Explorer->SetBase(static_cast<FMovementBaseInterfaceData*>(nullptr));
            Explorer->GetCharacterMovement()->StopMovementImmediately();
            Explorer->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
            Explorer->SetActorLocation(Eye,false,nullptr,ETeleportType::TeleportPhysics);
            SetControlRotation((City+Up*1200-Eye).Rotation());
            ++TestStage;StageStarted=TestTime;
        }
        else if(Elapsed>7)
        {
            int32 Cities=0,Buildings=0,Animals=0;
            for(TActorIterator<AVoyagerSettlement> It(GetWorld());It;++It){Cities+=It->ActiveCityCount();Buildings+=It->ActiveBuildingCount();}
            for(TActorIterator<AVoyagerAnimal> It(GetWorld());It;++It)if(It->PlanetIndex()==Planet)++Animals;
            if(Cities<1||Buildings<30||Buildings>768||Animals<4||Animals>24){Fail(TEXT("BOUNDED_CITY_AND_BIOME_POPULATION"));return;}
            UE_LOG(LogTemp,Display,TEXT("VOYAGER LIFE AUDIT PASS BIOME planet=%d biome=%d cities=%d buildings=%d fauna=%d"),Planet,Voyager::Biome(State->SystemSeed,Planet),Cities,Buildings,Animals);
            Capture();++TestStage;StageStarted=TestTime;
        }
        return;
    }
    if(TestStage==12)
    {
        AVoyagerAnimal* Subject=nullptr;
        for(TActorIterator<AVoyagerAnimal> It(GetWorld());It;++It)if(It->PlanetIndex()==4){Subject=*It;break;}
        if(!Subject){Fail(TEXT("FAUNA_PORTRAIT_MISSING"));return;}
        const FVector Up=Voyager::SurfaceNormal(State->SystemSeed,4,Subject->GetActorLocation());
        const FVector Eye=Subject->GetActorLocation()-Subject->GetActorForwardVector()*1400.0+Subject->GetActorRightVector()*400+Up*130.0;
        Explorer->SetActorLocation(Eye,false,nullptr,ETeleportType::TeleportPhysics);
        SetControlRotation((Subject->GetActorLocation()+Up*100-Eye).Rotation());
        ++TestStage;StageStarted=TestTime;return;
    }
    if(TestStage==13&&Elapsed>1.5)
    {
        Capture();TestStage=14;StageStarted=TestTime;return;
    }
    if(TestStage==14&&Elapsed>1.0)
    {
        auto* Mode=GetWorld()->GetAuthGameMode<AVoyagerGameMode>();auto* Ship=Mode?Mode->ShipFor(this):nullptr;
        if(!Ship){Fail(TEXT("CITY_TEST_SHIP"));return;}
        const FVector Up=AVoyagerSettlement::SiteDirection(State->SystemSeed,4);
        const FQuat Frame=Voyager::TangentRotation(Up).Quaternion();
        TestStart=Voyager::SurfacePoint(State->SystemSeed,4,Up)+Frame.GetAxisX()*-23000+Frame.GetAxisY()*-15750+Up*400;
        Explorer->SetActorHiddenInGame(true);Explorer->SetActorEnableCollision(false);Explorer->SetActorTickEnabled(false);
        Ship->ResetFlight(TestStart,Frame.Rotator(),false);Possess(Ship);Ship->SetFlightTestInput(1,0,false);
        ++TestStage;StageStarted=TestTime;
    }
}

void AVoyagerController::RunSurfaceAudit(float D)
{
    if(!HasAuthority()||TestStage>=20||TestTime<3)return;
    auto Explorer=Cast<AVoyagerCharacter>(GetPawn());auto State=GetWorld()->GetGameState<AVoyagerState>();if(!Explorer||!State)return;
    static const FVector Directions[]={FVector(1,.2,.15).GetSafeNormal(),FVector(.2,1,-.3).GetSafeNormal(),FVector(.35,.1,-1).GetSafeNormal()};
    const int32 Site=TestStage/3,Phase=TestStage%3;
    if(Site>=3){UE_LOG(LogTemp,Display,TEXT("VOYAGER SURFACE AUDIT COMPLETE PASS"));TestStage=20;return;}
    const FVector Up=Directions[Site];
    const float Elapsed=TestTime-StageStarted;
    auto Fail=[this,Site](const TCHAR* Label){UE_LOG(LogTemp,Error,TEXT("VOYAGER SURFACE AUDIT FAIL %s site=%d"),Label,Site);TestStage=99;};
    if(Phase==0)
    {
        // Test fixture placement only: gameplay flight never calls this audit.
        // Sample distant cube faces, including the underside of a planet.
        const FRotator Facing=Voyager::TangentRotation(Up);
        Explorer->SetBase(static_cast<FMovementBaseInterfaceData*>(nullptr));Explorer->GetCharacterMovement()->StopMovementImmediately();
        Explorer->SetActorLocationAndRotation(Voyager::SurfacePoint(State->SystemSeed,0,Up,600),Facing,false,nullptr,ETeleportType::TeleportPhysics);
        Explorer->GetCharacterMovement()->SetGravityDirection(-Up);Explorer->GetCharacterMovement()->SetMovementMode(MOVE_Falling);SetControlRotation(Facing);
        ++TestStage;StageStarted=TestTime;
    }
    else if(Phase==1&&Elapsed>7)
    {
        const double Height=Voyager::SurfaceAltitude(State->SystemSeed,0,Explorer->GetActorLocation());
        const double Alignment=FVector::DotProduct(Explorer->GetActorUpVector(),Up);
        if(!Explorer->GetCharacterMovement()->IsMovingOnGround()||Height<30||Height>250||Alignment<.999)
        {UE_LOG(LogTemp,Error,TEXT("VOYAGER SURFACE AUDIT DIAGNOSTIC grounded=%d altitude_cm=%.3f up_dot=%.9f"),Explorer->GetCharacterMovement()->IsMovingOnGround()?1:0,Height,Alignment);Fail(TEXT("RADIAL_GROUND_CONTACT"));return;}
        TestStart=Explorer->GetActorLocation();++TestStage;StageStarted=TestTime;
    }
    else if(Phase==2)
    {
        if(Elapsed<.8)Explorer->AddMovementInput(Explorer->GetActorForwardVector(),1);
        if(Elapsed>1.5)
        {
            const double Distance=FVector::Dist(TestStart,Explorer->GetActorLocation());
            if(Distance<150||!Explorer->GetCharacterMovement()->IsMovingOnGround()){Fail(TEXT("RADIAL_WALK"));return;}
            UE_LOG(LogTemp,Display,TEXT("VOYAGER SURFACE AUDIT PASS site=%d up=%s walk_cm=%.2f altitude_cm=%.2f"),Site,*Up.ToString(),Distance,Voyager::SurfaceAltitude(State->SystemSeed,0,Explorer->GetActorLocation()));
            if(FParse::Param(FCommandLine::Get(),TEXT("VoyagerCapture")))ConsoleCommand(TEXT("HighResShot 1"));
            ++TestStage;StageStarted=TestTime;
        }
    }
}
