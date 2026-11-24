// multi_gpu_status.h
//
// Copyright 2024 N-GINN LLC. All rights reserved.
//
#pragma once

#include <cstdint>

namespace drv3d_dx12
{
    enum class MultiGpuFallbackReason
    {
        driverNotInitialized,
        disabledByConfig,
        noSecondaryAdapter,
        adapterEnumerationFailed,
        deviceCreationFailed,
        capabilityQueryFailed,
        secondaryDeviceRemoved,
        transferFailed,
        available
    };

    inline const char* multi_gpu_fallback_reason_to_string(MultiGpuFallbackReason reason)
    {
        switch (reason)
        {
            case MultiGpuFallbackReason::driverNotInitialized:
                return "driver not initialized";
            case MultiGpuFallbackReason::disabledByConfig:
                return "disabled by config";
            case MultiGpuFallbackReason::noSecondaryAdapter:
                return "no suitable secondary GPU";
            case MultiGpuFallbackReason::adapterEnumerationFailed:
                return "secondary adapter enumeration failed";
            case MultiGpuFallbackReason::deviceCreationFailed:
                return "secondary device creation failed";
            case MultiGpuFallbackReason::capabilityQueryFailed:
                return "cross-adapter capability query failed";
            case MultiGpuFallbackReason::secondaryDeviceRemoved:
                return "secondary device removed";
            case MultiGpuFallbackReason::transferFailed:
                return "secondary transfer failed or unsupported";
            case MultiGpuFallbackReason::available:
                return "available";
        }
        return "unknown fallback reason";
    }

    inline MultiGpuFallbackReason check_secondary_gpu_health(MultiGpuFallbackReason current, uint32_t removal_reason)
    {
        // HRESULT's high bit denotes failure. Latch it until explicit driver recovery.
        return current == MultiGpuFallbackReason::available && (removal_reason & 0x80000000u) != 0
                   ? MultiGpuFallbackReason::secondaryDeviceRemoved
                   : current;
    }
}  // namespace drv3d_dx12
