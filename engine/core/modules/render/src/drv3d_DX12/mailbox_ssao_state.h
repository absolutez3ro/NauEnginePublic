// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once

#include "mailbox_ssao_resources.h"
#include "multi_gpu_fence.h"
#include <cstring>

namespace drv3d_dx12
{
    // CPU owns slot metadata only. GPU0 chooses a published payload at execution in the GPU variant.
    class MailboxSsaoState
    {
    public:
        MailboxSsaoResources resources;
        CrossAdapterFence inputsReady, resultsReady;
        bool gpuSelection = true;
        struct Input { uint64_t readDone = 0; } inputs[MAILBOX_IN_SLOTS];
        struct Output
        {
            uint64_t inputValue = 0, dataValue = 0, publishValue = 0, frame = 0, freeAfter = 0;
        } outputs[MAILBOX_OUT_SLOTS];
        struct Readback { uint64_t frame = 0, ready = 0; bool pending = false; } samples[MailboxSsaoResources::READBACK_SLOTS];
        int newestSlot = -1;
        uint64_t newestFrame = 0;
        d3d::mgpu::SsaoGpuTiming observations;

        HRESULT init(ID3D12Device* primary, ID3D12Device* secondary, const DeviceSsaoPassDesc& desc, bool gpu)
        {
            gpuSelection = gpu;
            observations.mailbox = true;
            observations.gpuMailbox = gpu;
            HRESULT result = resources.init(primary, secondary, desc);
            if (SUCCEEDED(result)) result = inputsReady.init(primary, secondary);
            if (SUCCEEDED(result)) result = resultsReady.init(secondary, primary);
            return result;
        }

        bool poll()
        {
            const uint64_t f0 = inputsReady.completedValue(), f1 = resultsReady.completedValue();
            if (f0 == UINT64_MAX || f1 == UINT64_MAX) return false;
            for (UINT i = 0; i < MAILBOX_OUT_SLOTS; ++i)
                if (outputs[i].frame > newestFrame && outputs[i].publishValue && f1 >= outputs[i].publishValue)
                {
                    newestFrame = outputs[i].frame;
                    newestSlot = static_cast<int>(i);
                }
            for (UINT i = 0; i < MAILBOX_OUT_SLOTS; ++i)
            {
                auto& output = outputs[i];
                if (!output.frame || static_cast<int>(i) == newestSlot || output.frame >= newestFrame || f1 < output.publishValue) continue;
                // Latch once. A future GPU0 signal includes every receive recorded before this observation.
                if (!output.freeAfter) output.freeAfter = inputsReady.issuedValue() + 1;
                if (f0 >= output.freeAfter) output = {};
            }
            for (UINT i = 0; i < MailboxSsaoResources::READBACK_SLOTS; ++i)
            {
                auto& sample = samples[i];
                if (!sample.pending || f0 < sample.ready) continue;
                uint32_t id = 0;
                std::memcpy(&id, resources.mappedReadback() + i * MailboxSsaoResources::READBACK_STRIDE, sizeof(id));
                if (sample.frame > observations.displayedFrame)
                {
                    observations.displayedFrame = sample.frame;
                    observations.displayedAoFrame = id;
                }
                if (!id) ++observations.unavailableAoFrames;
                else if (id > sample.frame) ++observations.badFrameIds;
                else
                {
                    const uint64_t age = sample.frame - id;
                    ++observations.ageHistogram[age < 7 ? age : 7];
                    ++observations.ageSamples;
                }
                sample.pending = false;
            }
            return true;
        }

        int freeInput() const
        {
            const uint64_t completed = resultsReady.completedValue();
            for (UINT i = 0; i < MAILBOX_IN_SLOTS; ++i)
                if (completed >= inputs[i].readDone) return static_cast<int>(i);
            return -1;
        }
        int reusableOutput(uint64_t inputValue) const
        {
            if (!inputValue || inputValue > inputsReady.issuedValue()) return -1;
            for (UINT i = 0; i < MAILBOX_OUT_SLOTS; ++i)
                if (!outputs[i].frame) return static_cast<int>(i);
            // GPU1 already waits for inputValue before executing the new job. That same F0
            // signal follows the old GPU0 receives, so a retired slot can be reserved now
            // without waiting for the CPU to observe their completion. Keep the newest
            // published slot pinned, and prefer the oldest eligible retired payload.
            int oldest = -1;
            for (UINT i = 0; i < MAILBOX_OUT_SLOTS; ++i)
            {
                const auto& output = outputs[i];
                if (static_cast<int>(i) != newestSlot && output.freeAfter && output.freeAfter <= inputValue &&
                    (oldest < 0 || output.frame < outputs[oldest].frame))
                    oldest = static_cast<int>(i);
            }
            return oldest;
        }
        UINT runnableJobs() const
        {
            const uint64_t f0 = inputsReady.completedValue(), f1 = resultsReady.completedValue();
            UINT count = 0;
            for (const auto& output : outputs)
                if (output.frame && f0 >= output.inputValue && f1 < output.dataValue) ++count;
            return count;
        }
        void copyObservations(d3d::mgpu::SsaoGpuTiming& timing) const
        {
            timing.mailbox = true;
            timing.gpuMailbox = gpuSelection;
            timing.displayedFrame = observations.displayedFrame;
            timing.displayedAoFrame = observations.displayedAoFrame;
            timing.ageSamples = observations.ageSamples;
            for (UINT i = 0; i < 8; ++i) timing.ageHistogram[i] = observations.ageHistogram[i];
            timing.skippedInput = observations.skippedInput;
            timing.skippedOutput = observations.skippedOutput;
            timing.skippedBacklog = observations.skippedBacklog;
            timing.submitOnlySignals = observations.submitOnlySignals;
            timing.badFrameIds = observations.badFrameIds;
            timing.unavailableAoFrames = observations.unavailableAoFrames;
            timing.droppedAgeSamples = observations.droppedAgeSamples;
        }
    };
}
