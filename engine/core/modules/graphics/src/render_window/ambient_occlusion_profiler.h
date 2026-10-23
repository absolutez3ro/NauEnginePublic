// ambient_occlusion_profiler.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once

#include <EASTL/shared_ptr.h>

#include "nau/3d/multi_gpu_ssao_types.h"

namespace nau::render
{
    eastl::shared_ptr<d3d::mgpu::ISsaoProfiler> createSsaoProfiler();
    eastl::shared_ptr<d3d::mgpu::ITransferProfiler> createSsaoTransferProfiler(const char* name, bool fromPrimary);
}  // namespace nau::render
