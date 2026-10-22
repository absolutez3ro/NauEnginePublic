// Copyright 2024 N-GINN LLC. All rights reserved.
#pragma once

#include <cstdint>

namespace drv3d_dx12
{
enum class MultiGpuFallbackReason
{
  DriverNotInitialized,
  DisabledByConfig,
  NoSecondaryAdapter,
  AdapterEnumerationFailed,
  DeviceCreationFailed,
  CapabilityQueryFailed,
  SecondaryDeviceRemoved,
  TransferFailed,
  Available
};

inline const char *multi_gpu_fallback_reason_to_string(MultiGpuFallbackReason reason)
{
  switch (reason)
  {
    case MultiGpuFallbackReason::DriverNotInitialized: return "driver not initialized";
    case MultiGpuFallbackReason::DisabledByConfig: return "disabled by config";
    case MultiGpuFallbackReason::NoSecondaryAdapter: return "no suitable secondary GPU";
    case MultiGpuFallbackReason::AdapterEnumerationFailed: return "secondary adapter enumeration failed";
    case MultiGpuFallbackReason::DeviceCreationFailed: return "secondary device creation failed";
    case MultiGpuFallbackReason::CapabilityQueryFailed: return "cross-adapter capability query failed";
    case MultiGpuFallbackReason::SecondaryDeviceRemoved: return "secondary device removed";
    case MultiGpuFallbackReason::TransferFailed: return "secondary transfer failed or unsupported";
    case MultiGpuFallbackReason::Available: return "available";
  }
  return "unknown fallback reason";
}

inline MultiGpuFallbackReason check_secondary_gpu_health(MultiGpuFallbackReason current, uint32_t removal_reason)
{
  // HRESULT's high bit denotes failure. Latch it until explicit driver recovery.
  return current == MultiGpuFallbackReason::Available && (removal_reason & 0x80000000u) != 0
    ? MultiGpuFallbackReason::SecondaryDeviceRemoved : current;
}
}
