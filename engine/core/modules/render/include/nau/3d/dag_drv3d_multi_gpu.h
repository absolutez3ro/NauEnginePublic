// dag_drv3d_multi_gpu.h
//
// Copyright 2024 N-GINN LLC. All rights reserved.
// Shared transfer channels between the primary engine device and a secondary device.
#pragma once

#include <cstdint>

#include "nau/3d/dag_drv3d.h"

class BaseTexture;

namespace d3d::mgpu
{
    struct ForeignQueueMeasurements
    {
        bool submitOnly = true;
        uint32_t releases = 0, waits = 0, frameFlushes = 0, slotWaits = 0;
        double cpuMilliseconds = 0, slotWaitMilliseconds = 0;
        uint32_t frameSlotWaits = 0;
        double frameSlotWaitMilliseconds = 0;
    };

    /**
     * @brief Configures foreign queue handoffs and resets their counters at the start of a render frame.
     * @param submitOnly Submit queued commands without retiring the current frontend frame slot.
     * @param measure Collect CPU timing for the handoffs in this frame.
     */
    NAU_RENDER_EXPORT void begin_foreign_queue_frame(bool submitOnly, bool measure);
    /** @brief Returns the handoff counters and timings for the current render frame. */
    NAU_RENDER_EXPORT ForeignQueueMeasurements get_foreign_queue_measurements();

}  // namespace d3d::mgpu
