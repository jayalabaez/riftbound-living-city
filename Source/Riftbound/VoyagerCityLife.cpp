#include "VoyagerCityLife.h"
#include "VoyagerGameMode.h"
#include "VoyagerCharacter.h"
#include "VoyagerCitizen.h"
#include "VoyagerSettlement.h"
#include "VoyagerData.h"
#include "VoyagerLaw.h"
#include "livingcity/sim/PlanetaryCity.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Compression.h"
#include "Misc/Base64.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Net/UnrealNetwork.h"
#include <atomic>

namespace
{
    constexpr int32 CityCount=Voyager::PlanetCount*AVoyagerSettlement::SitesPerPlanet;
    constexpr int32 FirstVisitor=3596;
    constexpr int32 MaxCityArchiveBytes=4*1024*1024;
    constexpr int32 CityCompressionHeaderBytes=8;
    bool DecodeCityArchive(const FVoyagerCityArchive& Archive,TArray<uint8>& Raw)
    {
        Raw.Reset();TArray<uint8> Packed;
        const TArray<uint8>* Encoded=&Archive.State;
        if(!Archive.PackedState.IsEmpty())
        {
            // Bound before Base64 allocates, and reject ambiguous mixed encodings.
            if(!Archive.State.IsEmpty()||Archive.PackedState.Len()>MaxCityArchiveBytes*2||
                !FBase64::Decode(Archive.PackedState,Packed))return false;
            Encoded=&Packed;
        }
        if(Encoded->IsEmpty()||Encoded->Num()>MaxCityArchiveBytes)return false;
        const bool Wrapped=Encoded->Num()>=4&&(*Encoded)[0]=='V'&&(*Encoded)[1]=='C'&&(*Encoded)[2]=='Z'&&(*Encoded)[3]=='1';
        if(!Wrapped)
        {
            if(!Archive.PackedState.IsEmpty())return false;
            Raw=*Encoded;return true; // Original opaque core delta saves remain valid.
        }
        if(Encoded->Num()<=CityCompressionHeaderBytes)return false;
        const uint32 Size=uint32((*Encoded)[4])|(uint32((*Encoded)[5])<<8)|(uint32((*Encoded)[6])<<16)|(uint32((*Encoded)[7])<<24);
        if(Size<100||Size>uint32(MaxCityArchiveBytes))return false;
        Raw.SetNumUninitialized(int32(Size));
        if(!FCompression::UncompressMemory(NAME_Zlib,Raw.GetData(),int64(Size),Encoded->GetData()+CityCompressionHeaderBytes,int64(Encoded->Num()-CityCompressionHeaderBytes)))
        {Raw.Reset();return false;}
        return true;
    }
    void EncodeCityArchive(FVoyagerCityArchive& Archive,std::span<const uint8> Raw)
    {
        Archive.State.Reset();Archive.PackedState.Reset();
        if(Raw.empty()||Raw.size()>std::size_t(MaxCityArchiveBytes))
        {
            UE_LOG(LogTemp,Error,TEXT("VOYAGER SAVE rejected oversized city delta bytes=%llu"),uint64(Raw.size()));return;
        }
        const int32 Size=int32(Raw.size());
        int32 Capacity=FCompression::CompressMemoryBound(NAME_Zlib,Size);
        TArray<uint8> Packed;
        if(Capacity>0&&Capacity<=MaxCityArchiveBytes-CityCompressionHeaderBytes)
        {
            Packed.SetNumUninitialized(Capacity+CityCompressionHeaderBytes);
            Packed[0]='V';Packed[1]='C';Packed[2]='Z';Packed[3]='1';
            for(int32 I=0;I<4;++I)Packed[4+I]=uint8(uint32(Size)>>(I*8));
            if(FCompression::CompressMemory(NAME_Zlib,Packed.GetData()+CityCompressionHeaderBytes,Capacity,Raw.data(),Size,COMPRESS_BiasSpeed))
            {
                Packed.SetNum(Capacity+CityCompressionHeaderBytes,EAllowShrinking::No);
                Archive.PackedState=FBase64::Encode(Packed);return;
            }
        }
        // A compressor failure must not discard a valid city. The legacy representation
        // remains readable, with an explicit warning about the slower fallback path.
        UE_LOG(LogTemp,Warning,TEXT("VOYAGER SAVE city compression unavailable; preserving raw delta."));
        Archive.State.Append(Raw.data(),Size);
    }
    FString Credits(int64 Minor){return FString::Printf(TEXT("%lld.%02lld cr"),Minor/100,FMath::Abs(Minor%100));}
    FString BuildingTitle(int32 Index)
    {
        static const TCHAR* Names[]={TEXT("Horizon Cafe"),TEXT("Frontier Clinic"),TEXT("Orbital Market"),TEXT("Shipwright Workshop"),TEXT("Explorer Residences"),TEXT("Civic Security")};
        return Index<0?TEXT("Outside"):FString::Printf(TEXT("%s %02d"),Names[Index%6],Index+1);
    }
    const TCHAR* ResultText(lc::PlanetaryResult Result)
    {
        switch(Result){
        case lc::PlanetaryResult::Accepted:return TEXT("Transaction completed.");
        case lc::PlanetaryResult::WrongLocation:return TEXT("Enter the appropriate building first. Your home and employer are listed in the phone.");
        case lc::PlanetaryResult::InsufficientFunds:return TEXT("Not enough credits. Work at your employer or sell mined minerals at a market.");
        case lc::PlanetaryResult::OutOfStock:return TEXT("This shop is out of stock. Check another market.");
        case lc::PlanetaryResult::NoInventory:return TEXT("You do not have that item. Buy supplies at a cafe or orbital market.");
        case lc::PlanetaryResult::NoJob:return TEXT("Apply for a job inside a workplace first.");
        case lc::PlanetaryResult::UtilitiesDisconnected:return TEXT("Utilities disconnected. Pay outstanding bills before using this service.");
        case lc::PlanetaryResult::Dead:return TEXT("This resident cannot act.");
        default:return TEXT("That action is unavailable.");}
    }
    lc::PlanetaryCityConfig ReadConfig()
    {
        lc::PlanetaryCityConfig C;C.population=3600;C.startMinute=540;
        FString Text;TSharedPtr<FJsonObject> Json;
        if(!FFileHelper::LoadFileToString(Text,*(FPaths::ProjectContentDir()/TEXT("CityData/economy.json")))||
            !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Json)||!Json.IsValid())
        {UE_LOG(LogTemp,Warning,TEXT("VOYAGER CITY LIFE using built-in economy defaults; data file unavailable."));return C;}
        auto Number=[&](const TCHAR* Key,int64 Default,int64 Min,int64 Max){double V=0;return Json->TryGetNumberField(Key,V)&&FMath::IsFinite(V)&&V>=Min&&V<=Max?int64(V):Default;};
        C.ticksPerMinute=uint32(Number(TEXT("ticksPerMinute"),20,20,1200));
        C.startMinute=uint32(Number(TEXT("startMinute"),540,0,1439));
        C.initialCitizenCashMinor=Number(TEXT("initialCitizenCashMinor"),20000,1000,1000000);
        C.hourlyWageMinor=Number(TEXT("hourlyWageMinor"),1800,100,100000);
        C.dailyRentMinor=Number(TEXT("dailyRentMinor"),900,0,100000);
        C.dailyUtilitiesMinor=Number(TEXT("dailyUtilitiesMinor"),180,0,100000);
        C.salesTaxBasisPoints=int32(Number(TEXT("salesTaxBasisPoints"),1000,0,5000));
        C.incomeTaxBasisPoints=int32(Number(TEXT("incomeTaxBasisPoints"),1000,0,5000));
        const TArray<TSharedPtr<FJsonValue>>* Prices=nullptr;
        if(Json->TryGetArrayField(TEXT("basePricesMinor"),Prices)&&Prices->Num()==8)
            for(int32 I=0;I<8;++I){double P=0;if((*Prices)[I]->TryGetNumber(P)&&FMath::IsFinite(P)&&P>=1&&P<=1000000)C.basePricesMinor[I]=int64(P);}
        return C;
    }
    TUniquePtr<lc::PlanetaryCity> GenerateArchiveCity(int32 System,int32 Planet,int32 Site,const lc::PlanetaryCityConfig& Config)
    {
        if(System<1||Planet<0||Planet>=Voyager::PlanetCount||Site<0||Site>=AVoyagerSettlement::SitesPerPlanet)return nullptr;
        std::vector<lc::PlanetaryBuilding> Layout;Layout.reserve(AVoyagerSettlement::BuildingCount());
        for(int32 Index=0;Index<AVoyagerSettlement::BuildingCount();++Index)
        {
            FVoyagerBuildingInfo B;if(!AVoyagerSettlement::GetBuildingInfo(System,Planet,Site,Index,B))continue;
            lc::PlanetaryBuildingKind Kind=lc::PlanetaryBuildingKind::Work;
            switch(B.Role){case 0:case 2:Kind=lc::PlanetaryBuildingKind::Shop;break;case 1:Kind=lc::PlanetaryBuildingKind::Clinic;break;case 4:Kind=lc::PlanetaryBuildingKind::Home;break;case 5:Kind=lc::PlanetaryBuildingKind::Civic;break;default:break;}
            Layout.push_back({uint32(Index),Kind});
        }
        auto City=MakeUnique<lc::PlanetaryCity>();
        const uint64 Seed=(uint64(uint32(System))<<32)|uint32(Voyager::Hash(uint32(Voyager::PlanetSeed(System,Planet))^uint32(Site+1)*73856093u));
        if(!City->Generate(Seed,Layout,Config))return nullptr;
        return City;
    }
    bool FillResidentView(const lc::PlanetaryCity& City,int32 Ordinal,FVoyagerCitizenLifeView& Out)
    {
        Out={};const auto* C=Ordinal>=0?City.Citizen(uint32(Ordinal)):nullptr;if(!C)return false;
        Out.bReady=true;Out.bAlive=C->alive;Out.Home=int32(C->home);Out.Workplace=int32(C->work);Out.GoalBuilding=int32(C->goalBuilding);
        Out.Activity=int32(C->activity);Out.Health=FMath::Clamp(100-int32(C->needs[4])/10,0,100);
        Out.ShiftStart=C->shiftStartMinute/60;Out.ShiftEnd=C->shiftEndMinute/60;Out.MoneyMinor=City.Balance(uint32(Ordinal));Out.FineMinor=C->fineMinor;
        for(auto N:C->needs)Out.Needs.Add(int32(N));return true;
    }
}

/** The only worker state is pure C++ city data and ordered commands. No actor is touched here. */
class FVoyagerCityLifeHost final : public FRunnable
{
public:
    enum class EInput : uint8 { Action,Presence,Controlled,Embodied,Health,Offense,Recovery,FieldCare,PlayerDamage };
    struct FInput { EInput Type;int32 City=0,Citizen=0,Value=0,Extra=0;uint64 Id=0;lc::PlanetaryCommand Command; };
    struct FResult {uint64 Id;lc::PlanetaryResult Result;};
    mutable FCriticalSection Mutex;
    TArray<TUniquePtr<lc::PlanetaryCity>> Cities;
    TArray<FInput> Inputs;
    TArray<FResult> Results;
    FRunnableThread* Thread=nullptr;
    std::atomic<bool> bStopping{false};
    float TickMs=0;
    uint64 NextId=1;
    uint64 ProcessedInput=0;
    explicit FVoyagerCityLifeHost(int32 System,const TArray<FVoyagerCityArchive>& Saved)
    {
        const auto Config=ReadConfig();Cities.Reserve(CityCount);Inputs.Reserve(512);Results.Reserve(32);
        for(int32 Key=0;Key<CityCount;++Key)
        {
            auto City=GenerateArchiveCity(System,Key/3,Key%3,Config);
            if(!City)UE_LOG(LogTemp,Fatal,TEXT("Planetary settlement has no valid housing, jobs or shops."));
            for(const auto& Archive:Saved)if(Archive.System==System&&Archive.Planet==Key/3&&Archive.Site==Key%3&&(!Archive.State.IsEmpty()||!Archive.PackedState.IsEmpty()))
            {
                TArray<uint8> Raw;
                if(!DecodeCityArchive(Archive,Raw)||!City->LoadDelta(std::span<const uint8>(Raw.GetData(),Raw.Num())))
                    UE_LOG(LogTemp,Error,TEXT("VOYAGER CITY LIFE rejected incompatible/corrupt city save system=%d city=%d"),System,Key);
                break;
            }
            // Presentation/controller attachment is rebuilt from the live world on load.
            for(uint32 I=0;I<City->CitizenCount();++I){City->SetEmbodied(I,false);City->SetControlled(I,false);}
            Cities.Add(MoveTemp(City));
        }
    }
    ~FVoyagerCityLifeHost(){Stop();if(Thread){Thread->WaitForCompletion();delete Thread;}}
    void Start(){Thread=FRunnableThread::Create(this,TEXT("VoyagerCitySimulation"),0,TPri_BelowNormal);checkf(Thread,TEXT("Unable to start the city simulation worker."));}
    virtual void Stop() override{bStopping.store(true);}
    uint64 Submit(FInput Input)
    {
        FScopeLock Lock(&Mutex);if(Inputs.Num()>=512)return 0;
        if(Input.Type==EInput::FieldCare)
        {
            const auto* Resident=Cities.IsValidIndex(Input.City)?Cities[Input.City]->Citizen(Input.Citizen):nullptr;
            if(!Resident||!Resident->alive)return 0;
        }
        Input.Id=NextId++;Inputs.Add(Input);return Input.Id;
    }
    void Flush()
    {
        // The game thread is the sole producer. Wait for its bounded command batch to
        // reach a fixed worker tick before saving or replacing the current system.
        while(Thread&&!bStopping.load())
        {
            {FScopeLock Lock(&Mutex);if(Inputs.IsEmpty())return;}
            FPlatformProcess::Sleep(.001f);
        }
    }
    virtual uint32 Run() override
    {
        double Next=FPlatformTime::Seconds();
        while(!bStopping.load())
        {
            const double Now=FPlatformTime::Seconds();
            if(Now<Next){FPlatformProcess::Sleep(.002f);continue;}
            const double StartTime=FPlatformTime::Seconds();
            {
                FScopeLock Lock(&Mutex);
                for(const FInput& Input:Inputs)
                {
                    if(!Cities.IsValidIndex(Input.City)){if(Input.Type==EInput::Action)Results.Add({Input.Id,lc::PlanetaryResult::InvalidCitizen});continue;}
                    auto& City=*Cities[Input.City];auto Row=City.Citizen(Input.Citizen);if(!Row){if(Input.Type==EInput::Action)Results.Add({Input.Id,lc::PlanetaryResult::InvalidCitizen});continue;}
                    switch(Input.Type)
                    {
                    case EInput::Action:{auto Cmd=Input.Command;Cmd.citizen=Input.Citizen;Cmd.sequence=Row->lastSequence+1;Results.Add({Input.Id,City.Apply(Cmd)});break;}
                    case EInput::Presence:City.SetPresence(Input.Citizen,uint32(Input.Value));break;
                    case EInput::Controlled:City.SetControlled(Input.Citizen,Input.Value!=0);break;
                    case EInput::Embodied:City.SetEmbodied(Input.Citizen,Input.Value!=0);break;
                    case EInput::Health:{const int32 Wanted=FMath::Clamp(100-Input.Value,0,100)*10;const int32 Difference=Wanted-Row->needs[4];if(Difference>0)City.Damage(Input.Citizen,uint16(Difference));break;}
                    // Player damage is relative so queued healing between two hits
                    // is not overwritten by a later, stale pawn-health value.
                    case EInput::PlayerDamage:City.Damage(Input.Citizen,uint16(Input.Value));break;
                    case EInput::Offense:City.RecordOffense(Input.Citizen,Input.Value>=100?lc::PlanetaryOffense::Homicide:Input.Value>=35?lc::PlanetaryOffense::PatrolAttack:Input.Value==25?lc::PlanetaryOffense::PropertyDamage:lc::PlanetaryOffense::Assault,uint16(Input.Extra),Row->lastEvidenceId+1);break;
                    case EInput::Recovery:City.EmergencyRecovery(Input.Citizen);break;
                    case EInput::FieldCare:City.RelieveNeeds(Input.Citizen,uint16(Input.Value),uint16(Input.Extra&1023),uint16((Input.Extra>>10)&1023));break;
                    }
                }
                // BuildView reads this watermark and needs under this same mutex.
                if(!Inputs.IsEmpty())ProcessedInput=Inputs.Last().Id;
                Inputs.Reset();for(auto& City:Cities)City->Step();
                TickMs=float((FPlatformTime::Seconds()-StartTime)*1000.0);
            }
            Next+=.05;
            // Scheduling slows under sustained load; never vary or skip a simulation step.
            if(Now-Next>1.0)Next=Now;
        }
        return 0;
    }
    void Capture(int32 System,TArray<FVoyagerCityArchive>& Out) const
    {
        FScopeLock Lock(&Mutex);
        Out.RemoveAll([&](const auto& Item){return Item.System==System;});
        for(int32 Key=0;Key<Cities.Num();++Key)
        {
            const auto Bytes=Cities[Key]->SaveDelta();FVoyagerCityArchive Item;Item.System=System;Item.Planet=Key/3;Item.Site=Key%3;
            EncodeCityArchive(Item,std::span<const uint8>(Bytes.data(),Bytes.size()));Out.Add(MoveTemp(Item));
        }
    }
};

void FVoyagerCityLifeHostDeleter::operator()(FVoyagerCityLifeHost* Host) const { delete Host; }
AVoyagerCityLife::AVoyagerCityLife()
{PrimaryActorTick.bCanEverTick=true;PrimaryActorTick.TickInterval=.1f;bReplicates=true;bAlwaysRelevant=true;SetNetUpdateFrequency(2.f);}
AVoyagerCityLife::~AVoyagerCityLife()=default;
void AVoyagerCityLife::BeginPlay(){Super::BeginPlay();}
AVoyagerCityLife* AVoyagerCityLife::Find(const UWorld* World)
{if(World)for(TActorIterator<AVoyagerCityLife> It(World);It;++It)return *It;return nullptr;}
void AVoyagerCityLife::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{Super::GetLifetimeReplicatedProps(OutLifetimeProps);DOREPLIFETIME(AVoyagerCityLife,ReplicatedHour);DOREPLIFETIME(AVoyagerCityLife,ReplicatedPopulation);}
void AVoyagerCityLife::EndPlay(const EEndPlayReason::Type Reason)
{
    // Capture before the worker/identities disappear, including window-close paths.
    // GameMode joins the one writer, so no detached job can overwrite a newer world.
    if(HasAuthority()&&Host)if(auto GM=GetWorld()->GetAuthGameMode<AVoyagerGameMode>())GM->SaveExpedition(true,this);
    Host.Reset();Super::EndPlay(Reason);
}
bool AVoyagerCityLife::IsReady() const{return Host.IsValid();}
void AVoyagerCityLife::RestoreFrom(const UVoyagerSave* Save)
{if(Save){Archives=Save->CityArchives;ResidentLocations=Save->CityResidentLocations;}ResidentLocations.SetNum(4);}
void AVoyagerCityLife::SaveTo(UVoyagerSave* Save)
{
    TUniqueFunction<TArray<FVoyagerCityArchive>()> Encode;
    if(TryPrepareSave(Save,Encode,true))Save->CityArchives=Encode();
}
bool AVoyagerCityLife::TryPrepareSave(UVoyagerSave* Save,TUniqueFunction<TArray<FVoyagerCityArchive>()>& Encode,bool bWaitForCommands)
{
    if(!HasAuthority()||!Save)return false;
    if(auto State=GetWorld()->GetGameState<AVoyagerState>())
        if(!Host||ActiveSystem!=State->SystemSeed)
        {
            if(!bWaitForCommands)return false;
            RefreshSystem(State->SystemSeed);
        }
    if(!Host)return false;
    if(bWaitForCommands)Host->Flush();
    std::vector<lc::PlanetaryCity> Copies;Copies.reserve(Host->Cities.Num());
    TArray<FVoyagerCityLifeHost::FResult> Completed;
    const double Started=FPlatformTime::Seconds();
    {
        // Checking the pending batch, draining its results and cloning the core are
        // atomic. A failed trade's refund is settled below before GameMode reads cargo.
        FScopeLock Lock(&Host->Mutex);
        if(!Host->Inputs.IsEmpty())return false;
        Completed=MoveTemp(Host->Results);Host->Results.Reset();
        for(const auto& City:Host->Cities)Copies.push_back(*City);
    }
    const double CloneMs=(FPlatformTime::Seconds()-Started)*1000.0;
    for(const auto& Result:Completed)SettleAction(Result.Id,uint8(Result.Result));
    Save->CityResidentLocations=ResidentLocations;
    TArray<FVoyagerCityArchive> Previous=Archives;
    const int32 System=ActiveSystem;
    UE_LOG(LogTemp,Display,TEXT("VOYAGER SAVE CAPTURE system=%d cities=%d clone_gt_ms=%.3f settled_results=%d"),System,int32(Copies.size()),CloneMs,Completed.Num());
    Encode=[Copies=MoveTemp(Copies),Previous=MoveTemp(Previous),System]() mutable
    {
        const double EncodeStarted=FPlatformTime::Seconds();
        Previous.RemoveAll([System](const auto& Item){return Item.System==System;});
        // Migrate retained systems from old raw saves on this same background job.
        // Otherwise their reflected byte arrays would preserve the original hitch.
        for(auto& Archive:Previous)if(Archive.PackedState.IsEmpty()&&!Archive.State.IsEmpty())
        {
            TArray<uint8> Raw;
            if(DecodeCityArchive(Archive,Raw))EncodeCityArchive(Archive,std::span<const uint8>(Raw.GetData(),Raw.Num()));
        }
        uint64 RawBytes=0,PackedChars=0,LegacyBytes=0;
        for(int32 Key=0;Key<int32(Copies.size());++Key)
        {
            const auto Bytes=Copies[Key].SaveDelta();FVoyagerCityArchive Item;Item.System=System;Item.Planet=Key/3;Item.Site=Key%3;
            RawBytes+=Bytes.size();EncodeCityArchive(Item,std::span<const uint8>(Bytes.data(),Bytes.size()));Previous.Add(MoveTemp(Item));
        }
        for(const auto& Archive:Previous){PackedChars+=Archive.PackedState.Len();LegacyBytes+=Archive.State.Num();}
        UE_LOG(LogTemp,Display,TEXT("VOYAGER SAVE ENCODE system=%d background=%d encode_ms=%.3f raw_bytes=%llu packed_chars=%llu legacy_bytes=%llu"),System,!IsInGameThread(),(FPlatformTime::Seconds()-EncodeStarted)*1000.0,RawBytes,PackedChars,LegacyBytes);
        return MoveTemp(Previous);
    };
    return true;
}
void AVoyagerCityLife::RefreshSystem(int32 System)
{
    if(Host){Host->Flush();SettleActions();Host->Stop();Host->Thread->WaitForCompletion();}
    TUniquePtr<FVoyagerCityLifeHost,FVoyagerCityLifeHostDeleter> NextHost(new FVoyagerCityLifeHost(System,Archives));
    if(Host)
    {
        const auto State=GetWorld()->GetGameState<AVoyagerState>();const int32 Arrival=State?State->PlanetIndex*3:0;
        for(int32 Slot=0;Slot<ResidentLocations.Num();++Slot)
        {
            const int32 Source=FMath::Clamp(ResidentLocations[Slot],0,CityCount-1);
            if(const auto* Resident=Host->Cities[Source]->Citizen(FirstVisitor+Slot);Resident&&!Resident->alive)
                Host->Cities[Source]->EmergencyRecovery(FirstVisitor+Slot);
            if(Host->Cities[Source]->SwapResident(*NextHost->Cities[Arrival],FirstVisitor+Slot,FirstVisitor+Slot))ResidentLocations[Slot]=Arrival;
        }
        Host->Capture(ActiveSystem,Archives);Host.Reset();
    }
    PendingPlayerHealth.Reset();PendingRecovery.Reset();LastCoreHealth.Reset();LastCorePawn.Reset();
    ActiveSystem=System;Host=MoveTemp(NextHost);Host->Start();
    if(ResidentLocations.Num()!=4)ResidentLocations.SetNumZeroed(4);
    for(int32& Key:ResidentLocations)Key=FMath::Clamp(Key,0,CityCount-1);
    ReplicatedPopulation=CityCount*3600;
    UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY LIFE READY system=%d cities=%d residents=%d thread=worker fixed_hz=20"),System,CityCount,ReplicatedPopulation);
}
int32 AVoyagerCityLife::PlayerOrdinal(AController* Controller) const
{const int32* Slot=PlayerSlots.Find(Controller);return Slot?FirstVisitor+*Slot:FirstVisitor;}
int32 AVoyagerCityLife::PlayerCityKey(AController* Controller) const
{
    const int32* Slot=PlayerSlots.Find(Controller);return Slot&&ResidentLocations.IsValidIndex(*Slot)?ResidentLocations[*Slot]:0;
}
bool AVoyagerCityLife::GetCitizen(int32 System,int32 Planet,int32 Site,int32 Ordinal,FVoyagerCitizenLifeView& Out) const
{
    Out={};if(!Host||System!=ActiveSystem||Planet<0||Planet>=5||Site<0||Site>=3)return false;
    FScopeLock Lock(&Host->Mutex);return FillResidentView(*Host->Cities[Planet*3+Site],Ordinal,Out);
}
bool AVoyagerCityLife::ValidateArchive(const FVoyagerCityArchive& Archive,int32 Ordinal,FVoyagerCitizenLifeView& OutResident,int64& TotalMinor,int64& IssuedMinor)
{
    OutResident={};TotalMinor=0;IssuedMinor=0;
    auto City=GenerateArchiveCity(Archive.System,Archive.Planet,Archive.Site,ReadConfig());
    if(!City)return false;
    TArray<uint8> Raw;
    if(!DecodeCityArchive(Archive,Raw)||!City->LoadDelta(std::span<const uint8>(Raw.GetData(),Raw.Num()))||!City->ConservationHolds())return false;
    const auto Stats=City->Stats();TotalMinor=Stats.totalMoneyMinor;IssuedMinor=Stats.issuedMoneyMinor;
    return TotalMinor==IssuedMinor&&FillResidentView(*City,Ordinal,OutResident);
}
bool AVoyagerCityLife::IsCitizenAlive(int32 System,int32 Planet,int32 Site,int32 Ordinal) const
{FVoyagerCitizenLifeView V;return !GetCitizen(System,Planet,Site,Ordinal,V)||V.bAlive;}
void AVoyagerCityLife::SetEmbodied(int32 System,int32 Planet,int32 Site,int32 Ordinal,bool bEmbodied)
{if(Host&&System==ActiveSystem)Host->Submit({FVoyagerCityLifeHost::EInput::Embodied,Planet*3+Site,Ordinal,bEmbodied?1:0});}
void AVoyagerCityLife::ReportPresence(int32 System,int32 Planet,int32 Site,int32 Ordinal,int32 Building)
{if(Host&&System==ActiveSystem)Host->Submit({FVoyagerCityLifeHost::EInput::Presence,Planet*3+Site,Ordinal,Building});}
void AVoyagerCityLife::ApplyCitizenDamage(int32 System,int32 Planet,int32 Site,int32 Ordinal,int32 HealthRemaining)
{if(Host&&System==ActiveSystem)Host->Submit({FVoyagerCityLifeHost::EInput::Health,Planet*3+Site,Ordinal,HealthRemaining});}
void AVoyagerCityLife::RecordCrime(AController* Offender,int32 Severity,int32 Confidence)
{if(Host&&Offender&&PlayerSlots.Contains(Offender))Host->Submit({FVoyagerCityLifeHost::EInput::Offense,PlayerCityKey(Offender),PlayerOrdinal(Offender),Severity,Confidence});}
void AVoyagerCityLife::ApplyPlayerDamage(AController* Controller,float DamageApplied,float HealthRemaining)
{
    if(!HasAuthority()||!Host||!Controller||!PlayerSlots.Contains(Controller)||!FMath::IsFinite(DamageApplied)||DamageApplied<=0||!FMath::IsFinite(HealthRemaining))return;
    const int32 Pressure=FMath::Clamp(FMath::RoundToInt(FMath::Min(DamageApplied,100.f)*10.f),1,1000);
    const FVoyagerCityLifeHost::FInput Input{FVoyagerCityLifeHost::EInput::PlayerDamage,PlayerCityKey(Controller),PlayerOrdinal(Controller),Pressure};
    uint64 Id=Host->Submit(Input);
    // Health must not be dropped if the bounded input queue is temporarily full.
    if(!Id){Host->Flush();Id=Host->Submit(Input);}
    if(Id)PendingPlayerHealth.Add(Controller,{FMath::Clamp(HealthRemaining,0.f,100.f),Id});
}
void AVoyagerCityLife::RecoverPlayer(AController* Controller)
{
    if(!HasAuthority()||!Host||!Controller||!PlayerSlots.Contains(Controller))return;
    const FVoyagerCityLifeHost::FInput Input{FVoyagerCityLifeHost::EInput::Recovery,PlayerCityKey(Controller),PlayerOrdinal(Controller)};
    uint64 Id=Host->Submit(Input);if(!Id){Host->Flush();Id=Host->Submit(Input);}
    if(Id){PendingPlayerHealth.Remove(Controller);PendingRecovery.Add(Controller,Id);}
}
bool AVoyagerCityLife::ApplyFieldCare(AController* Controller,int32 Food,int32 Water,int32 Health)
{
    if(!HasAuthority()||!Host||!Controller||!PlayerSlots.Contains(Controller)||Food<0||Food>1000||Water<0||Water>1000||Health<0||Health>1000)return false;
    auto* Explorer=Cast<AVoyagerCharacter>(Controller->GetPawn());
    if(!Explorer||!FMath::IsFinite(Explorer->Health)||Explorer->Health<=0.f)return false;
    if(auto Law=AVoyagerLaw::Find(GetWorld());Law&&Law->IsJailed(Controller))return false;
    const uint64 Id=Host->Submit({FVoyagerCityLifeHost::EInput::FieldCare,PlayerCityKey(Controller),PlayerOrdinal(Controller),Food,Water|(Health<<10)});
    if(!Id)return false;
    if(Health>0)
    {
        // The authority pawn already includes every local hit and accepted heal.
        // Project this accepted command immediately so a following hit cannot use
        // pre-treatment health and incorrectly trigger emergency recovery.
        Explorer->Health=FMath::Clamp(Explorer->Health+Health*.1f,0.f,100.f);
        PendingPlayerHealth.Add(Controller,{Explorer->Health,Id});Explorer->ForceNetUpdate();
    }
    return true;
}
bool AVoyagerCityLife::AuditConservation(int64& Total,int64& Issued) const
{
    Total=0;Issued=0;if(!Host)return false;FScopeLock Lock(&Host->Mutex);bool Valid=true;
    for(auto& City:Host->Cities){const auto S=City->Stats();Total+=S.totalMoneyMinor;Issued+=S.issuedMoneyMinor;Valid=Valid&&City->ConservationHolds();}
    return Valid&&Total==Issued;
}

FVoyagerCityLifeView AVoyagerCityLife::BuildView(AController* Controller) const
{
    FVoyagerCityLifeView V;if(!Host||!Controller||!Controller->GetPawn())return V;
    const int32 Key=PlayerCityKey(Controller),Ordinal=PlayerOrdinal(Controller);const FVector Position=Controller->GetPawn()->GetActorLocation();
    V.bAvailable=true;V.CityKey=Key;V.ResidentId=Ordinal;V.bOnFoot=Cast<AVoyagerCharacter>(Controller->GetPawn())!=nullptr;
    const FVector Center=Voyager::SurfacePoint(ActiveSystem,Key/3,AVoyagerSettlement::SiteDirection(ActiveSystem,Key/3,Key%3));
    V.bAtCity=FVector::DistSquared(Position,Center)<FMath::Square(85000.);
    V.CurrentBuilding=V.bAtCity?AVoyagerSettlement::FindBuildingAt(ActiveSystem,Key/3,Key%3,Position):INDEX_NONE;
    V.CurrentBuildingName=BuildingTitle(V.CurrentBuilding);V.CityName=AVoyagerSettlement::SiteName(ActiveSystem,Key/3,Key%3);
    FScopeLock Lock(&Host->Mutex);const auto& City=*Host->Cities[Key];const auto C=City.Citizen(Ordinal);if(!C)return {};
    const auto Stats=City.Stats();V.Tick=int64(Stats.tick);V.ProcessedInput=Host->ProcessedInput;V.Population=int32(Stats.living);V.SystemPopulation=ReplicatedPopulation;
    V.CreditsMinor=City.Balance(Ordinal);V.GovernmentMinor=Stats.governmentMinor;V.WageMinor=City.Config().hourlyWageMinor;
    V.RentMinor=City.Config().dailyRentMinor;V.BillsMinor=C->billsDueMinor;V.FineMinor=C->fineMinor;V.CriminalRecord=int32(C->convictions);
    V.bEmployed=C->work!=lc::kPlanetaryInvalidBuilding;V.bRented=C->home!=lc::kPlanetaryInvalidBuilding;V.bUtilitiesConnected=C->utilitiesConnected;
    V.bWorking=C->activity==lc::PlanetaryActivity::Working;V.HomeBuilding=int32(C->home);V.WorkplaceBuilding=int32(C->work);
    V.HomeName=BuildingTitle(V.HomeBuilding);V.WorkName=BuildingTitle(V.WorkplaceBuilding);V.Waste=int32(C->waste);V.TickMilliseconds=Host->TickMs;
    V.ClockText=FString::Printf(TEXT("Day %llu  %02u:%02u"),1+Stats.tick/(City.Config().ticksPerMinute*1440ull),Stats.minuteOfDay/60,Stats.minuteOfDay%60);
    V.ShiftStatus=V.bWorking?TEXT("Working here / wages credited each completed hour"):TEXT("Enter your employer and start a shift to earn wages");
    V.LegalStatus=C->fineMinor>0?TEXT("Outstanding citation / pay at Civic Security"):TEXT("No outstanding city fines");
    for(auto N:C->needs)V.Needs.Add(int32(N));
    int32 Market=V.CurrentBuilding;
    if(Market<0||(Market%6!=0&&Market%6!=2))Market=2;
    static const TCHAR* Goods[]={TEXT("Food"),TEXT("Water"),TEXT("Fuel"),TEXT("Raw materials"),TEXT("Building materials"),TEXT("Consumer goods"),TEXT("Electronics"),TEXT("Medicine")};
    for(int32 Good=0;Good<8;++Good)
    {FVoyagerCityGoodView Item;Item.Name=Goods[Good];Item.PriceMinor=City.Price(Market,lc::PlanetaryGood(Good));Item.Stock=int32(City.Stock(Market,lc::PlanetaryGood(Good)));Item.Owned=C->inventory[Good];V.Goods.Add(Item);}
    V.Bulletin.Add(FString::Printf(TEXT("%s reports %u living residents. %u are seeking work."),*V.CityName,Stats.living,Stats.unemployed));
    V.Bulletin.Add(FString::Printf(TEXT("City accounts: %s in the public treasury. Local employers have paid %s in wages."),*Credits(Stats.governmentMinor),*Credits(Stats.wagesPaidMinor)));
    V.Bulletin.Add(FString::Printf(TEXT("Supply desk: food %s, water %s. Prices include sales tax and change with stock."),*Credits(V.Goods[0].PriceMinor),*Credits(V.Goods[1].PriceMinor)));
    V.Bulletin.Add(FString::Printf(TEXT("Sanitation: %u waste units awaiting collection; %u units returned to the materials supply."),Stats.waste,Stats.recycledUnits));
    V.Bulletin.Add(TEXT("Public notice: carry food and water when leaving the city. P opens your city phone anywhere; local services require the right building."));
    return V;
}

void AVoyagerCityLife::HandleAction(AController* Controller,uint8 Action,int32 Argument)
{
    if(!HasAuthority()||!Host||!Controller||!PlayerSlots.Contains(Controller)||Action>uint8(EVoyagerCityAction::Recycle))return;
    auto PC=Cast<AVoyagerController>(Controller);auto State=GetWorld()->GetGameState<AVoyagerState>();if(!PC||!State||State->bTransitioning)return;
    if(auto Law=AVoyagerLaw::Find(GetWorld());Law&&Law->IsJailed(Controller)){PC->Notify(TEXT("Civic services resume after your sentence."));return;}
    const double Now=GetWorld()->GetTimeSeconds();if(Now-LastCommandTimes.FindRef(Controller)<.2)return;LastCommandTimes.Add(Controller,Now);
    const auto View=BuildView(Controller);if(!View.bAvailable)return;
    const auto Type=EVoyagerCityAction(Action);
    const bool Remote=Type==EVoyagerCityAction::ConsumeGood;
    if(!Remote&&(!View.bAtCity||!View.bOnFoot)){PC->Notify(TEXT("Land and enter a city building to use this service."));return;}
    if(!Remote&&View.CurrentBuilding>=0)
        if(auto Destruction=AVoyagerDestruction::Find(GetWorld()))
            if(const auto Damage=Destruction->FindDamage(State->SystemSeed,View.CityKey/3,View.CityKey%3,View.CurrentBuilding))
            {
                FVoyagerBuildingInfo Building;
                if(!AVoyagerSettlement::GetBuildingInfo(State->SystemSeed,View.CityKey/3,View.CityKey%3,View.CurrentBuilding,Building))
                {PC->Notify(TEXT("Building services are unavailable."));return;}
                // Regional support loss can remove every floor while aggregate
                // integrity remains positive. Services follow the surviving structure.
                if(AVoyagerDestruction::SurvivingFloors(Damage,Building.FloorCount)<=0)
                {PC->Notify(TEXT("This building has collapsed. Visit another branch for city services."));return;}
            }
    lc::PlanetaryCommand Cmd;Cmd.building=uint32(View.CurrentBuilding);Cmd.quantity=1;
    switch(Type)
    {
    case EVoyagerCityAction::BuyGood:Cmd.type=lc::PlanetaryCommandType::Purchase;break;
    case EVoyagerCityAction::ConsumeGood:Cmd.type=lc::PlanetaryCommandType::Consume;break;
    case EVoyagerCityAction::ApplyJob:Cmd.type=lc::PlanetaryCommandType::ApplyJob;break;
    case EVoyagerCityAction::StartWork:Cmd.type=lc::PlanetaryCommandType::StartWork;break;
    case EVoyagerCityAction::StopWork:Cmd.type=lc::PlanetaryCommandType::StopWork;break;
    case EVoyagerCityAction::Rest:Cmd.type=lc::PlanetaryCommandType::Rest;break;
    case EVoyagerCityAction::Wash:Cmd.type=lc::PlanetaryCommandType::Wash;break;
    case EVoyagerCityAction::PayBills:Cmd.type=lc::PlanetaryCommandType::PayBills;break;
    case EVoyagerCityAction::RentHome:Cmd.type=lc::PlanetaryCommandType::RentHome;break;
    case EVoyagerCityAction::PayFine:Cmd.type=lc::PlanetaryCommandType::PayFine;break;
    case EVoyagerCityAction::SellMinerals:Cmd.type=lc::PlanetaryCommandType::SellRawMaterials;Cmd.quantity=10;break;
    case EVoyagerCityAction::Recycle:Cmd.type=lc::PlanetaryCommandType::Recycle;break;
    }
    if(Type==EVoyagerCityAction::BuyGood||Type==EVoyagerCityAction::ConsumeGood)
    {if(Argument<0||Argument>=8)return;Cmd.good=lc::PlanetaryGood(Argument);}
    if(Type==EVoyagerCityAction::PayFine&&(View.CurrentBuilding<0||View.CurrentBuilding%6!=5)){PC->Notify(TEXT("Visit a Civic Security building to settle your citation."));return;}
    int32 Minerals=0;
    if(Type==EVoyagerCityAction::SellMinerals)
    {
        auto PS=Controller->GetPlayerState<AVoyagerPlayerState>();
        if(!PS||PS->Minerals<10){PC->Notify(TEXT("This trade requires 10 mined minerals in your cargo."));return;}Minerals=10;
    }
    const int32 Key=PlayerCityKey(Controller),Ordinal=PlayerOrdinal(Controller);
    Host->Submit({FVoyagerCityLifeHost::EInput::Presence,Key,Ordinal,View.CurrentBuilding});
    FVoyagerCityLifeHost::FInput Input{FVoyagerCityLifeHost::EInput::Action,Key,Ordinal};Input.Command=Cmd;
    auto PS=Controller->GetPlayerState<AVoyagerPlayerState>();
    if(Minerals&&PS){PS->Minerals-=Minerals;PS->ForceNetUpdate();}
    const uint64 Id=Host->Submit(Input);
    if(Id)PendingActions.Add(Id,{Controller,Type,Minerals,PS});
    else if(Minerals&&PS){PS->Minerals+=Minerals;PS->ForceNetUpdate();}
}

bool AVoyagerCityLife::SettleActions()
{
    if(!Host)return false;
    TArray<FVoyagerCityLifeHost::FResult> Results;
    {FScopeLock Lock(&Host->Mutex);Results=MoveTemp(Host->Results);Host->Results.Reset();}
    bool bSave=false;for(const auto& Result:Results)bSave=SettleAction(Result.Id,uint8(Result.Result))||bSave;
    return bSave;
}
bool AVoyagerCityLife::SettleAction(uint64 Id,uint8 RawResult)
{
    const auto Result=lc::PlanetaryResult(RawResult);auto Pending=PendingActions.Find(Id);if(!Pending)return false;
    const bool bAccepted=Result==lc::PlanetaryResult::Accepted;
    if(!bAccepted&&Pending->Minerals)
        if(auto PS=Pending->PlayerState.Get()){PS->Minerals=FMath::Min(100000000,PS->Minerals+Pending->Minerals);PS->ForceNetUpdate();}
    if(auto C=Pending->Controller.Get())
    {
        if(bAccepted&&Pending->Action==EVoyagerCityAction::PayFine)if(auto Law=AVoyagerLaw::Find(GetWorld()))Law->Resolve(C);
        if(auto PC=Cast<AVoyagerController>(C))PC->Notify(ResultText(Result));
    }
    PendingActions.Remove(Id);
    return bAccepted;
}
void AVoyagerCityLife::Tick(float D)
{
    Super::Tick(D);if(!HasAuthority())return;auto State=GetWorld()->GetGameState<AVoyagerState>();if(!State||State->bTransitioning)return;
    if(!Host||ActiveSystem!=State->SystemSeed)RefreshSystem(State->SystemSeed);
    bool bSave=SettleActions();
    SnapshotRemaining-=D;if(SnapshotRemaining>0){if(bSave)if(auto GM=GetWorld()->GetAuthGameMode<AVoyagerGameMode>())GM->SaveExpedition();return;}SnapshotRemaining=.5f;
    // A disconnected controller relinquishes the ordinary resident rather than
    // leaving it permanently controlled, unable to buy food or resume its routine.
    for(auto It=PlayerSlots.CreateIterator();It;++It)if(!It.Key().IsValid())
    {
        const auto Key=It.Key();const int32 Slot=It.Value();
        if(ResidentLocations.IsValidIndex(Slot))Host->Submit({FVoyagerCityLifeHost::EInput::Controlled,ResidentLocations[Slot],FirstVisitor+Slot,0});
        LastCommandTimes.Remove(Key);LastCoreHealth.Remove(Key);LastCorePawn.Remove(Key);PendingPlayerHealth.Remove(Key);PendingRecovery.Remove(Key);
        It.RemoveCurrent();
    }
    for(FConstPlayerControllerIterator It=GetWorld()->GetPlayerControllerIterator();It;++It)
    {
        auto C=It->Get();if(!C||!C->GetPawn())continue;
        if(!PlayerSlots.Contains(C))
        {
            TSet<int32> Used;for(auto Pair:PlayerSlots)if(Pair.Key.IsValid())Used.Add(Pair.Value);
            int32 Slot=0;while(Used.Contains(Slot))++Slot;if(Slot>=4)continue;PlayerSlots.Add(C,Slot);
        }
        const int32 Ordinal=PlayerOrdinal(C);int32 Key=PlayerCityKey(C);
        if(Cast<AVoyagerCharacter>(C->GetPawn()))
        {
            const FVector Position=C->GetPawn()->GetActorLocation();const int32 Planet=Voyager::NearestPlanet(ActiveSystem,Position);
            for(int32 Site=0;Site<3;++Site)
            {
                const int32 Destination=Planet*3+Site;if(Destination==Key)continue;
                const FVector Center=Voyager::SurfacePoint(ActiveSystem,Planet,AVoyagerSettlement::SiteDirection(ActiveSystem,Planet,Site));
                if(FVector::DistSquared(Position,Center)>FMath::Square(70000.))continue;
                Host->Flush();bSave=SettleActions()||bSave;
                FScopeLock Lock(&Host->Mutex);
                if(Host->Cities[Key]->SwapResident(*Host->Cities[Destination],Ordinal,Ordinal))
                {
                    ResidentLocations[PlayerSlots.FindChecked(C)]=Destination;Key=Destination;
                    UE_LOG(LogTemp,Display,TEXT("VOYAGER CITY LIFE RESIDENCY player=%d city=%d credits=%lld"),Ordinal,Key,Host->Cities[Key]->Balance(Ordinal));
                }
                break;
            }
        }
        Host->Submit({FVoyagerCityLifeHost::EInput::Controlled,Key,Ordinal,1});
        const auto View=BuildView(C);Host->Submit({FVoyagerCityLifeHost::EInput::Presence,Key,Ordinal,View.CurrentBuilding});
        if(View.Needs.Num()==9)
        {
            float Health=FMath::Clamp(100.f-View.Needs[4]*.1f,0.f,100.f);
            if(const uint64* Recovery=PendingRecovery.Find(C))
            {if(View.ProcessedInput>=*Recovery)PendingRecovery.Remove(C);else Health=100;}
            if(const FPendingPlayerHealth* Pending=PendingPlayerHealth.Find(C))
            {if(View.ProcessedInput>=Pending->CommandId)PendingPlayerHealth.Remove(C);else Health=Pending->Health;}
            const float* Previous=LastCoreHealth.Find(C);
            const bool ChangedPawn=LastCorePawn.FindRef(C).Get()!=C->GetPawn();
            const auto Explorer=Cast<AVoyagerCharacter>(C->GetPawn());
            if(!Previous||*Previous!=Health||ChangedPawn||(Explorer&&!FMath::IsNearlyEqual(Explorer->Health,float(Health))))
            {
                LastCoreHealth.Add(C,Health);LastCorePawn.Add(C,C->GetPawn());
                if(Explorer)
                {
                    Explorer->Health=float(Health);Explorer->ForceNetUpdate();
                    if(Health<=0)if(auto GM=GetWorld()->GetAuthGameMode<AVoyagerGameMode>())GM->RecoverExplorer(Explorer);
                }
                else if(Health<=0)
                {
                    // Ordinary emergency care also applies during flight: restore the
                    // resident, not the ship transform, wallet or identity.
                    RecoverPlayer(C);
                    if(auto PC=Cast<AVoyagerController>(C))PC->Notify(TEXT("SHIP MEDICAL ASSIST / Life support restored. Carry food and water for long voyages."));
                }
            }
        }
        if(auto PC=Cast<AVoyagerController>(C))PC->ClientReceiveCityLife(View);
    }
    {FScopeLock Lock(&Host->Mutex);ReplicatedHour=float(Host->Cities[0]->MinuteOfDay())/60.f;}
    ForceNetUpdate();if(bSave)if(auto GM=GetWorld()->GetAuthGameMode<AVoyagerGameMode>())GM->SaveExpedition();
}
