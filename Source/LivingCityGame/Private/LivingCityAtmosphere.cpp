#include "LivingCityAtmosphere.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/Scene.h"

ALivingCityAtmosphere::ALivingCityAtmosphere()
{
    PrimaryActorTick.bCanEverTick = false;

    SceneRoot     = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    RootComponent = SceneRoot;

    // Every setter below is guarded by USceneComponent::AreDynamicDataChangesAllowed(), which
    // is unconditionally true for a component that is not yet registered - so calling them
    // from the constructor is exactly as safe as writing the properties directly, and keeps
    // to the one code path the engine maintains. The project runs r.AllowStaticLighting=False,
    // so everything is Movable: a Static or Stationary light here contributes nothing at all.

    // ---- sun ----------------------------------------------------------------------------
    Sun = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("Sun"));
    Sun->SetupAttachment(SceneRoot);
    Sun->SetMobility(EComponentMobility::Movable);
    Sun->SetRelativeRotation(FRotator(-46.0f, 30.0f, 0.0f));
    Sun->SetIntensity(6.5f);
    Sun->SetLightColor(FLinearColor(1.0f, 0.91f, 0.78f));
    Sun->SetCastShadows(true);
    Sun->SetAtmosphereSunLight(true);
    Sun->SetAtmosphereSunLightIndex(0);

    // ---- fill ---------------------------------------------------------------------------
    // UDirectionalLightComponent's constructor defaults bAtmosphereSunLight to TRUE, so the
    // fill has to opt out explicitly or the sky would be lit by two suns. Opposite yaw to the
    // sun at about the same elevation (pitch -50 against the sun's -46), cool and shadowless:
    // it lifts the shadow side without adding a second set of shadows to read.
    Fill = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("Fill"));
    Fill->SetupAttachment(SceneRoot);
    Fill->SetMobility(EComponentMobility::Movable);
    Fill->SetRelativeRotation(FRotator(-50.0f, 210.0f, 0.0f));
    Fill->SetIntensity(1.8f);
    Fill->SetLightColor(FLinearColor(0.37f, 0.54f, 0.82f));
    Fill->SetCastShadows(false);
    Fill->SetAtmosphereSunLight(false);

    // With volumetric fog on, the renderer wants ONE directional light for forward shading,
    // translucency and fog, and warns on screen every frame if two are equally eligible.
    // The sun wins; the fill is ambient bounce and never needs to be the fog light.
    Sun->SetForwardShadingPriority(1);
    Fill->SetForwardShadingPriority(0);

    // ---- sky atmosphere -----------------------------------------------------------------
    // TransformMode is left at its default, PlanetTopAtAbsoluteWorldOrigin: the ground is the
    // Z=0 plane and the planet curves away beneath it. Riftbound's
    // PlanetCenterAtComponentTransform is for a world that IS a sphere; used here it would put
    // the player 6,360 km above the atmosphere. Scattering numbers are Riftbound's.
    Atmosphere = CreateDefaultSubobject<USkyAtmosphereComponent>(TEXT("SkyAtmosphere"));
    Atmosphere->SetupAttachment(SceneRoot);
    Atmosphere->SetMobility(EComponentMobility::Movable);
    Atmosphere->SetRayleighExponentialDistribution(8.0f);
    Atmosphere->SetMieExponentialDistribution(1.2f);
    Atmosphere->SetMieScatteringScale(0.8f);
    Atmosphere->SetMultiScatteringFactor(1.5f);
    Atmosphere->SetSkyLuminanceFactor(FLinearColor(0.85f, 0.93f, 1.0f));

    // ---- clouds -------------------------------------------------------------------------
    // The material is deliberately untouched: the component's own constructor points it at
    // the engine's m_SimpleVolumetricCloud_Inst, which it loads on registration, and which is
    // written for exactly this flat-planet-top setup. Altitudes are in kilometres.
    Clouds = CreateDefaultSubobject<UVolumetricCloudComponent>(TEXT("Clouds"));
    Clouds->SetupAttachment(SceneRoot);
    Clouds->SetMobility(EComponentMobility::Movable);
    Clouds->SetLayerBottomAltitude(2.0f);
    Clouds->SetLayerHeight(4.0f);
    Clouds->SetSkyLightCloudBottomOcclusion(0.4f);
    Clouds->SetbUsePerSampleAtmosphericLightTransmittance(true);

    // ---- sky light ----------------------------------------------------------------------
    // bRealTimeCapture is written as a property rather than through SetRealTimeCapture(): the
    // setter also queues the component for capture, and a class-default object has no scene
    // to capture into. The flag is read when the render state is created on registration,
    // which is when the capture actually starts.
    SkyLight = CreateDefaultSubobject<USkyLightComponent>(TEXT("SkyLight"));
    SkyLight->SetupAttachment(SceneRoot);
    SkyLight->SetMobility(EComponentMobility::Movable);
    SkyLight->SetIntensity(1.2f);
    SkyLight->bRealTimeCapture = true;

    // ---- height fog ---------------------------------------------------------------------
    // Pale blue in-scattering so the far blocks fade toward the sky colour rather than
    // popping against it. Volumetric, so the sun shafts down the streets. The start distance
    // keeps the near street crisp.
    Fog = CreateDefaultSubobject<UExponentialHeightFogComponent>(TEXT("HeightFog"));
    Fog->SetupAttachment(SceneRoot);
    Fog->SetMobility(EComponentMobility::Movable);
    Fog->SetFogDensity(0.02f);
    Fog->SetFogHeightFalloff(0.2f);
    Fog->SetStartDistance(500.0f);
    Fog->SetVolumetricFog(true);
    Fog->SetFogInscatteringColor(FLinearColor(0.62f, 0.72f, 0.86f));

    // ---- exposure -----------------------------------------------------------------------
    // Exactly Riftbound's post settings, so a light intensity tuned there reads the same here.
    // Manual exposure with the physical-camera term off means the sun's 6.5 and the sky
    // light's 1.2 are taken literally rather than auto-normalised away.
    PostProcess = CreateDefaultSubobject<UPostProcessComponent>(TEXT("Exposure"));
    PostProcess->SetupAttachment(SceneRoot);
    PostProcess->bUnbound = true;
    PostProcess->Settings.bOverride_AutoExposureMethod                      = true;
    PostProcess->Settings.AutoExposureMethod                                = EAutoExposureMethod::AEM_Manual;
    PostProcess->Settings.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
    PostProcess->Settings.AutoExposureApplyPhysicalCameraExposure           = false;
    PostProcess->Settings.bOverride_AutoExposureBias                        = true;
    PostProcess->Settings.AutoExposureBias                                  = 0.35f;
    PostProcess->Settings.bOverride_BloomIntensity                          = true;
    PostProcess->Settings.BloomIntensity                                    = 0.28f;
    PostProcess->Settings.bOverride_VignetteIntensity                       = true;
    PostProcess->Settings.VignetteIntensity                                 = 0.16f;
    PostProcess->Settings.bOverride_MotionBlurAmount                        = true;
    PostProcess->Settings.MotionBlurAmount                                  = 0.0f;
}
