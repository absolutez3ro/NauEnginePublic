// Console check of the Mgpu <-> Nau transfer bridge (no Nau involved).
// GPU2 pattern -> shared heap -> GPU1 -> CPU, verified by a checksum for every frame.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "nau/mgpu/bridge/mgpu_bridge.h"

int main(int argc, char** argv)
{
    const uint32_t iterations = argc > 1 ? static_cast<uint32_t>(std::atoi(argv[1])) : 120;

    nau::mgpu::bridge::TransferBridge bridge;

    // nullptr: standalone process, the first hardware adapter becomes the primary one.
    if (!bridge.initialize(nullptr))
    {
        std::printf("Bridge initialization failed: %s\n", bridge.lastError());
        return 2;
    }

    std::printf("Primary adapter  : %s\n", bridge.primaryAdapterName());
    std::printf("Secondary adapter: %s\n", bridge.secondaryAdapterName());

    uint32_t failed = 0;
    for (uint32_t frame = 0; frame < iterations; ++frame)
    {
        if (!bridge.update(frame))
        {
            ++failed;
            std::printf("frame %u: FAILED (%s)\n", frame, bridge.lastError());
        }
        else if (frame % 30 == 0)
        {
            const auto& info = bridge.info();
            std::printf("frame %u: OK, %ux%u, checksum %016llx\n", frame, info.width, info.height,
                        static_cast<unsigned long long>(info.checksum));
        }
    }

    std::printf("%u of %u transfers failed\n", failed, iterations);
    bridge.shutdown();
    return failed == 0 ? 0 : 1;
}
