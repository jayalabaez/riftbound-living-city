#include "Modules/ModuleManager.h"

// LIVING CITY runtime module. Nothing to do at load time: the simulation is owned by
// ULivingCitySubsystem and starts with the world, not with the module.
IMPLEMENT_MODULE(FDefaultGameModuleImpl, LivingCityGame);
