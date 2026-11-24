// foreign_queue_measurements.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include "device.h"
#include "nau/3d/dag_drv3d_multi_gpu.h"

void d3d::mgpu::begin_foreign_queue_frame(bool submitOnly, bool measure)
{
    drv3d_dx12::get_device().getContext().beginForeignQueueFrame(submitOnly, measure);
}

d3d::mgpu::ForeignQueueMeasurements d3d::mgpu::get_foreign_queue_measurements()
{
    return drv3d_dx12::get_device().getContext().getForeignQueueMeasurements();
}
