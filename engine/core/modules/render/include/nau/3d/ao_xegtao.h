// ao_xegtao.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once
#include "nau/3d/ao_executor.h"
namespace nau::render::ao
{
    /** @brief Creates XeGTAO's native passes, without creating a device, queue or transport. */
    NAU_RENDER_EXPORT NativeAlgorithm* createXeGtaoAlgorithm();
}  // namespace nau::render::ao
