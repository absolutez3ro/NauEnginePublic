#pragma once

#include <cstdint>
#include <memory>

namespace nau::mgpu::bridge
{
    struct DofParameters
    {
        float focusDistance = 25.0f;
        float focusRange = 6.0f;
        float farZ = 1000.0f;
        float maxCoC = 3.4f;
        float filterRadius = 3.0f;
        float taps = 32.0f;
    };

    enum class DofResult
    {
        Completed,
        Fallback,
        PrimaryFailure
    };

    class DofBridge
    {
    public:
        DofBridge();
        ~DofBridge();
        DofBridge(const DofBridge&) = delete;
        DofBridge& operator=(const DofBridge&) = delete;

        bool prepare(void* primaryDevice, void* primaryQueue, uint32_t width, uint32_t height);
        DofResult execute(void* color, void* linearDepth, void* colorFilled, void* filtered, const DofParameters& parameters);
        const char* lastError() const;
        const char* secondaryAdapterName() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}  // namespace nau::mgpu::bridge
