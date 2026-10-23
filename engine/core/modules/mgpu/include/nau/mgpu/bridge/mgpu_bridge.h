#pragma once

// Facade between Nau and the Mgpu module.
//
// Only plain C++ types are exposed here on purpose: Mgpu headers must not be included into Nau code
// (global `using namespace DirectX::SimpleMath`, the `PI` macro, /permissive, static CRT).
//
// Transfer path (see mgpu_bridge.cpp):
//   GPU2: CPU pattern -> upload buffer -> texture -> ::GCrossAdapterResource (shared heap)
//   GPU1 (Mgpu device on Nau's adapter): shared heap -> read-back buffer -> CPU pixels
// The CPU pixels are then written by the caller into a Nau texture (BaseTexture::lockimg).
//
// Without NAU_ENABLE_MGPU (the Mgpu target defines it publicly) this header provides inline stubs, so Nau code
// can use the same calls whether or not Mgpu is linked.

#include <cstdint>
#include <memory>

namespace nau::mgpu::bridge
{
    struct TransferInfo
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t rowPitch = 0; // bytes per row of pixels(); pixels are tightly packed RGBA8
        uint64_t checksum = 0; // FNV-1a of the pixels received through the transfer path
        uint64_t expectedChecksum = 0; // FNV-1a of the pattern generated on the CPU for the same frame
        bool checksumMatches = false;
    };

#if defined(NAU_ENABLE_MGPU)

    /// Manual switch of the Mgpu path. Default: false.
    void setEnabled(bool enabled);
    bool isEnabled();

    class TransferBridge
    {
    public:
        TransferBridge();
        ~TransferBridge();

        TransferBridge(const TransferBridge&) = delete;
        TransferBridge& operator=(const TransferBridge&) = delete;

        /// nauAdapterName: name of the adapter Nau renders on (d3d::get_device_name()); it selects the primary
        /// device. Needs two distinct hardware adapters; returns false (see lastError()) otherwise.
        bool initialize(const char* nauAdapterName);

        /// Runs one full transfer and waits for it. frameIndex shifts the pattern so every call sends fresh data.
        /// Returns true when the transfer completed and the received pixels match the expected pattern.
        bool update(uint32_t frameIndex);

        bool isReady() const;
        void shutdown();

        const char* lastError() const;
        const char* primaryAdapterName() const;
        const char* secondaryAdapterName() const;

        /// Tightly packed RGBA8 pixels of the last transfer, info().width * info().height * 4 bytes.
        const void* pixels() const;
        const TransferInfo& info() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

#else

    inline void setEnabled(bool) {}
    inline bool isEnabled() { return false; }

    class TransferBridge
    {
    public:
        bool initialize(const char*) { return false; }
        bool update(uint32_t) { return false; }
        bool isReady() const { return false; }
        void shutdown() {}
        const char* lastError() const { return "Mgpu is not linked (NAU_ENABLE_MGPU is off)"; }
        const char* primaryAdapterName() const { return ""; }
        const char* secondaryAdapterName() const { return ""; }
        const void* pixels() const { return nullptr; }
        const TransferInfo& info() const { return m_info; }

    private:
        TransferInfo m_info;
    };

#endif
}
