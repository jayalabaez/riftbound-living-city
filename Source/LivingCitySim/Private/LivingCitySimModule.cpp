// UBT module entry point for LivingCitySim.
//
// Every Unreal module DLL must implement the module interface. Without this, the DLL loads
// and then Unreal reports:
//
//     The game module 'LivingCitySim' could not be successfully initialized after it was
//     loaded.
//
// This file, and this file alone in the module, is allowed to include an Unreal header: it
// is the UBT wrapper, not the simulation. Rule R1 governs the sources under <repo>/Sim,
// which is what Scripts/Check-SimPurity.ps1 actually scans. Nothing in Sim/ knows Unreal
// exists, and that stays true.
//
// FDefaultModuleImpl, not FDefaultGameModuleImpl: this module carries no gameplay and owns
// no UObjects. It is a plain C++ library that happens to be packaged as an Unreal module.

#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, LivingCitySim);
