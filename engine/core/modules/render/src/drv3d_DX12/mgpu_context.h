// mgpu_context.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once
#include "nau/3d/dag_drv3d.h"
#include "nau/mgpu/bridge/device_context.h"
namespace drv3d_dx12
{
    std::shared_ptr<nau::mgpu::DeviceContext> get_mgpu_device_context(d3d::GpuId gpu);
}
