// graphics_config.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
//
#pragma once

#ifdef NAU_GRAPHICS_EXPORT
    #undef NAU_GRAPHICS_EXPORT
#endif

#if defined(NAU_STATIC_RUNTIME)
    #define NAU_GRAPHICS_EXPORT
#elif defined(_MSC_VER)
    #if defined(NAU_GRAPHICS_BUILD)
        #define NAU_GRAPHICS_EXPORT __declspec(dllexport)
    #else
        #define NAU_GRAPHICS_EXPORT __declspec(dllimport)
    #endif
#else
    #error Unknown Compiler/OS
#endif
