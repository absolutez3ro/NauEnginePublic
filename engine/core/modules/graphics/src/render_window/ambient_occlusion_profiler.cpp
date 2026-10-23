// ambient_occlusion_profiler.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include "ambient_occlusion_profiler.h"

#include "gpu_profiler/d3d12_timestamp_source.h"

namespace nau::render
{
    namespace
    {
        class SsaoProfiler final : public d3d::mgpu::ISsaoProfiler
        {
        public:
            SsaoProfiler() = default;
            ~SsaoProfiler() override
            {
                detach();
            }

            void attach(void* device, void* queue) override
            {
                gpu_profiler::GpuProfiler& profiler = gpu_profiler::GpuProfiler::instance();
                if (!profiler.isEnabled() || profiler.hasGpu(gpu_profiler::SecondaryGpu))
                {
                    return;
                }
                if (m_source.init(static_cast<ID3D12Device*>(device), static_cast<ID3D12CommandQueue*>(queue)))
                {
                    m_registered = profiler.registerGpu(gpu_profiler::SecondaryGpu, &m_source);
                }
            }

            void beginPass(void* list, const char* name) override
            {
                if (m_registered)
                {
                    m_source.setCommandList(static_cast<ID3D12GraphicsCommandList*>(list));
                    gpu_profiler::GpuProfiler::instance().beginScope(name, gpu_profiler::SecondaryGpu);
                }
            }

            void endPass() override
            {
                if (m_registered)
                {
                    gpu_profiler::GpuProfiler::instance().endScope(gpu_profiler::SecondaryGpu);
                }
            }

            void resolve(void* list) override
            {
                if (m_registered)
                {
                    m_source.resolve(static_cast<ID3D12GraphicsCommandList*>(list));
                    m_source.setCommandList(nullptr);
                }
            }

            void submitted() override
            {
                if (m_registered)
                {
                    m_source.submitted();
                }
            }

            void detach() override
            {
                if (m_registered)
                {
                    gpu_profiler::GpuProfiler::instance().unregisterGpu(gpu_profiler::SecondaryGpu);
                    m_source.setCommandList(nullptr);
                    m_registered = false;
                }
            }

            eastl::shared_ptr<d3d::mgpu::ITransferProfiler> createTransferProfiler(const char* name, bool fromPrimary) override
            {
                return createSsaoTransferProfiler(name, fromPrimary);
            }

        private:
            gpu_profiler::D3D12TimestampSource m_source;
            bool m_registered = false;
        };
    }  // namespace

    eastl::shared_ptr<d3d::mgpu::ISsaoProfiler> createSsaoProfiler()
    {
        return gpu_profiler::GpuProfiler::instance().isEnabled() ? eastl::make_shared<SsaoProfiler>() : nullptr;
    }
}  // namespace nau::render
