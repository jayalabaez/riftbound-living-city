#pragma once
#include "CoreMinimal.h"

namespace Voyager
{
    inline uint32 Hash(uint32 Value){Value^=Value>>16;Value*=0x7feb352d;Value^=Value>>15;Value*=0x846ca68b;return Value^(Value>>16);}
    inline int32 PlanetSeed(int32 System,int32 Planet){return int32(Hash(uint32(System)*7919u+uint32(Planet)*104729u+4829u)&0x7fffffff);}
    // Each system contains all five climate families while the seed rotates their order.
    inline int32 Biome(int32 System,int32 Planet){return (PlanetSeed(System,0)%5+Planet)%5;}
    inline FString SystemName(int32 System)
    {
        static const TCHAR* Names[]={TEXT("LYRA"),TEXT("VESPER"),TEXT("ORION"),TEXT("KESTREL"),TEXT("NACRE"),TEXT("SOLACE"),TEXT("EMBER"),TEXT("AEGIS")};
        return FString::Printf(TEXT("%s-%05d"),Names[Hash(System)%8],System);
    }
    inline FString PlanetName(int32 System,int32 Planet)
    {
        static const TCHAR* Names[]={TEXT("Aurelia"),TEXT("Nivalis"),TEXT("Cinder"),TEXT("Viridia"),TEXT("Amethyst"),TEXT("Ilyra"),TEXT("Oceara"),TEXT("Solene"),TEXT("Veyra"),TEXT("Talos"),TEXT("Eos"),TEXT("Nyx")};
        return FString::Printf(TEXT("%s %c"),Names[uint32(PlanetSeed(System,Planet))%12],TCHAR('b'+Planet));
    }
    inline const TCHAR* BiomeName(int32 B)
    {
        static const TCHAR* Names[]={TEXT("VERDANT GARDEN"),TEXT("CRYSTAL DESERT"),TEXT("FROZEN FRONTIER"),TEXT("EMBER WASTES"),TEXT("VIOLET MYCELIUM")};return Names[FMath::Clamp(B,0,4)];
    }
    inline FLinearColor BiomeColor(int32 B)
    {
        static const FLinearColor Colors[]={FLinearColor(.13f,.44f,.28f),FLinearColor(.63f,.35f,.13f),FLinearColor(.32f,.64f,.74f),FLinearColor(.31f,.075f,.035f),FLinearColor(.38f,.13f,.52f)};return Colors[FMath::Clamp(B,0,4)];
    }
    constexpr int32 PlanetCount=5;
    constexpr double CentimetersPerKm=100000.0;
    constexpr double AtmosphereHeight=60.0*CentimetersPerKm;
    inline double PlanetRadius(int32 System,int32 Index)
    {return (600.0+double(PlanetSeed(System,Index)%301))*CentimetersPerKm;}
    inline FVector PlanetCenter(int32 System,int32 Index)
    {
        static const FVector CentersKm[]={FVector(0,0,0),FVector(5600,1600,500),
            FVector(-3000,6500,-1200),FVector(-6300,-2400,800),FVector(1800,-8200,1500)};
        return CentersKm[FMath::Clamp(Index,0,PlanetCount-1)]*CentimetersPerKm-FVector(0,0,PlanetRadius(System,0));
    }
    inline FVector SurfaceNormal(int32 System,int32 Index,const FVector& Position)
    {return (Position-PlanetCenter(System,Index)).GetSafeNormal(UE_SMALL_NUMBER,FVector::UpVector);}
    inline double TerrainHeight(int32 System,int32 Index,FVector Direction)
    {
        Direction=Direction.GetSafeNormal(UE_SMALL_NUMBER,FVector::UpVector);
        const uint32 Seed=uint32(PlanetSeed(System,Index));
        const FVector Offset(double(Seed%997)*.117,double(Hash(Seed)%991)*.131,double(Hash(Seed+37)%983)*.109);
        const FVector P=Direction*(PlanetRadius(System,Index)/2200000.0)+Offset;
        const double Continents=FMath::PerlinNoise3D(Direction*4.7+Offset)*95000.0;
        const double Mountains=FMath::PerlinNoise3D(P)*110000.0;
        const double Hills=FMath::PerlinNoise3D(P*4.13+FVector(9.1,17.3,-5.7))*17000.0;
        const double Detail=FMath::PerlinNoise3D(P*23.17+FVector(-3.7,2.3,8.1))*1500.0;
        // A small shared north-pole landing clearing; every other point uses the same
        // radial field for collision, low-altitude patches and the orbital silhouette.
        const double ArcDistance=(Direction-FVector::UpVector).Size()*PlanetRadius(System,Index);
        double Blend=FMath::Clamp((ArcDistance-80000.0)/520000.0,0.0,1.0);
        Blend=Blend*Blend*(3.0-2.0*Blend);
        return (Continents+Mountains+Hills+Detail)*Blend;
    }
    inline FVector SurfacePoint(int32 System,int32 Index,FVector Direction,double Clearance=0.0)
    {
        Direction=Direction.GetSafeNormal(UE_SMALL_NUMBER,FVector::UpVector);
        return PlanetCenter(System,Index)+Direction*(PlanetRadius(System,Index)+TerrainHeight(System,Index,Direction)+Clearance);
    }
    inline double SurfaceAltitude(int32 System,int32 Index,const FVector& Position)
    {
        const FVector Relative=Position-PlanetCenter(System,Index);
        return Relative.Size()-PlanetRadius(System,Index)-TerrainHeight(System,Index,Relative.GetSafeNormal());
    }
    inline int32 NearestPlanet(int32 System,const FVector& Position)
    {
        int32 Nearest=0;double Best=TNumericLimits<double>::Max();
        for(int32 I=0;I<PlanetCount;++I)
        {
            const double Distance=(Position-PlanetCenter(System,I)).Size()-PlanetRadius(System,I);
            if(Distance<Best){Best=Distance;Nearest=I;}
        }
        return Nearest;
    }
    inline float AtmosphereDensity(double Altitude)
    {
        const double H=FMath::Max(0.0,Altitude);
        return float(FMath::Exp(-H/800000.0)*FMath::Clamp((AtmosphereHeight-H)/1000000.0,0.0,1.0));
    }
    inline FRotator TangentRotation(FVector UnitUp,FVector ForwardHint=FVector::ForwardVector)
    {
        UnitUp=UnitUp.GetSafeNormal(UE_SMALL_NUMBER,FVector::UpVector);
        FVector Forward=FVector::VectorPlaneProject(ForwardHint,UnitUp).GetSafeNormal();
        if(Forward.IsNearlyZero())
        {
            const FVector Axis=FMath::Abs(UnitUp.Z)<.9?FVector::UpVector:FVector::ForwardVector;
            Forward=FVector::CrossProduct(Axis,UnitUp).GetSafeNormal();
        }
        return FRotationMatrix::MakeFromXZ(Forward,UnitUp).Rotator();
    }
}
