#include "multi_gpu_status.h"

#include <cstdlib>
#include <cstring>

int main()
{
  using namespace drv3d_dx12;
  using Reason = MultiGpuFallbackReason;
  const Reason reasons[] = {Reason::DriverNotInitialized, Reason::DisabledByConfig, Reason::NoSecondaryAdapter,
    Reason::AdapterEnumerationFailed, Reason::DeviceCreationFailed, Reason::CapabilityQueryFailed,
    Reason::SecondaryDeviceRemoved, Reason::TransferFailed, Reason::Available};
  const uint32_t results[] = {0u, 1u, 0x887a0005u, 0x887a0006u, 0x80004005u};
  for (const auto reason : reasons)
  {
    if (std::strlen(multi_gpu_fallback_reason_to_string(reason)) == 0)
      return EXIT_FAILURE;
    for (const auto result : results)
    {
      const auto expected = reason == Reason::Available && (result & 0x80000000u) != 0
        ? Reason::SecondaryDeviceRemoved : reason;
      if (check_secondary_gpu_health(reason, result) != expected)
        return EXIT_FAILURE;
    }
  }
  if (std::strcmp(multi_gpu_fallback_reason_to_string(Reason::NoSecondaryAdapter), "no suitable secondary GPU") != 0)
    return EXIT_FAILURE;
  // Recovery of a health probe alone must not re-enable a latched failure.
  auto state = check_secondary_gpu_health(Reason::Available, 0x887a0005u);
  state = check_secondary_gpu_health(state, 0u);
  return state == Reason::SecondaryDeviceRemoved ? EXIT_SUCCESS : EXIT_FAILURE;
}
