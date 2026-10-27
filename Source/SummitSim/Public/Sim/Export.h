#pragma once

// UBT defines SUMMITSIM_API as DLLEXPORT/DLLIMPORT for the SummitSim module; standalone CMake builds
// leave it empty. Those two macros normally come from UE's Platform.h, which the sim core must not
// include, so define them here with UE's exact spelling (identical redefinition is legal).
#ifndef DLLEXPORT
#if defined(_WIN32)
#define DLLEXPORT __declspec(dllexport)
#else
#define DLLEXPORT __attribute__((visibility("default")))
#endif
#endif
#ifndef DLLIMPORT
#if defined(_WIN32)
#define DLLIMPORT __declspec(dllimport)
#else
#define DLLIMPORT __attribute__((visibility("default")))
#endif
#endif

// std:: members in exported types are safe across our DLLs: both modules use the same toolchain and CRT.
#if defined(_MSC_VER)
#pragma warning(disable : 4251)
#endif

#ifndef SUMMITSIM_API
#define SUMMITSIM_API
#endif
