// mailbox_state.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once

#include <cstring>

#include "cross_adapter_fence.h"
#include "mailbox_resources.h"

namespace nau::mgpu
{
    /** @brief Tracks input, output and readback slot ownership on the render thread. */
    class MailboxState
    {
    public:
        MailboxResources resources;
        CrossAdapterFence inputsReady, resultsReady;
        struct Input
        {
            uint64_t readDone = 0;
        } inputs[MAILBOX_IN_SLOTS];
        struct Output
        {
            uint64_t inputValue = 0, dataValue = 0, publishValue = 0, frame = 0, freeAfter = 0;
        } outputs[MAILBOX_OUT_SLOTS];
        struct Readback
        {
            uint64_t frame = 0, ready = 0;
            bool pending = false;
        } samples[MailboxResources::READBACK_SLOTS];
        int newestSlot = -1;
        uint64_t newestFrame = 0;
        struct Observations
        {
            uint64_t displayedFrame = 0, displayedSourceFrame = 0, ageSamples = 0;
            uint64_t ageHistogram[8] = {};
            uint64_t skippedInput = 0, skippedOutput = 0, skippedBacklog = 0;
            uint64_t submitOnlySignals = 0, badFrameIds = 0, unavailableFrames = 0;
            uint64_t droppedAgeSamples = 0, inputCopyFrames = 0;
        } observations;

        struct Submission
        {
            uint64_t inputValue = 0;
            int input = -1, output = -1;
            bool accepted = false;
        };

        /** @brief Reserves a progress signal even when a frame is skipped. The caller must submit that signal. */
        Submission reserve()
        {
            Submission result;
            result.inputValue = inputsReady.reserveSignal();
            if (!result.inputValue)
            {
                return result;
            }
            result.input = freeInput();
            result.output = reusableOutput(result.inputValue);
            if (result.input < 0)
            {
                ++observations.skippedInput;
            }
            else if (result.output < 0)
            {
                ++observations.skippedOutput;
            }
            else if (runnableJobs() > 1)
            {
                ++observations.skippedBacklog;
            }
            else
            {
                result.accepted = true;
            }
            return result;
        }

        /** @brief Commits ownership only after both payload and publication were successfully queued. */
        void commit(const Submission& reservation, uint64_t dataReady, uint64_t published, uint64_t frame)
        {
            inputs[reservation.input].readDone = dataReady;
            outputs[reservation.output] = {reservation.inputValue, dataReady, published, frame, 0};
        }

        /** @brief Describes a GPU-selected receive; a future input signal retires its readback storage. */
        MailboxReceiveDesc receive(uint64_t frame, bool sampleAge)
        {
            MailboxReceiveDesc desc;
            desc.mailbox = resources.mailboxPrimary();
            desc.predicate = resources.predicate();
            desc.frameId = resources.frameId();
            for (UINT slot = 0; slot < MAILBOX_OUT_SLOTS; ++slot)
            {
                desc.payload[slot] = resources.output(slot, 0);
                desc.id[slot] = resources.outputId(slot, 0);
            }
            for (UINT i = 1; i < MAILBOX_OUT_SLOTS; ++i)
            {
                UINT j = i;
                while (j && outputs[desc.copyOrder[j]].frame < outputs[desc.copyOrder[j - 1]].frame)
                {
                    std::swap(desc.copyOrder[j], desc.copyOrder[j - 1]);
                    --j;
                }
            }
            if (sampleAge)
            {
                for (UINT slot = 0; slot < MailboxResources::READBACK_SLOTS; ++slot)
                {
                    if (!samples[slot].pending)
                    {
                        samples[slot] = {frame, inputsReady.issuedValue() + 1, true};
                        desc.readback = resources.readback();
                        desc.readbackOffset = slot * MailboxResources::READBACK_STRIDE;
                        break;
                    }
                }
                if (!desc.readback)
                {
                    ++observations.droppedAgeSamples;
                }
            }
            return desc;
        }

        HRESULT init(const std::shared_ptr<DeviceContext>& primary, const std::shared_ptr<DeviceContext>& secondary, const MailboxDescription& desc)
        {
            HRESULT result = resources.init(primary, secondary, desc);
            if (SUCCEEDED(result))
            {
                result = inputsReady.init(primary->device(), secondary->device());
            }
            if (SUCCEEDED(result))
            {
                result = resultsReady.init(secondary->device(), primary->device());
            }
            return result;
        }

        /**
         * @brief Retires completed work and reads available source age samples without waiting.
         * @return False if either fence reports device removal.
         */
        bool poll()
        {
            const uint64_t f0 = inputsReady.completedValue(), f1 = resultsReady.completedValue();
            if (f0 == UINT64_MAX || f1 == UINT64_MAX)
            {
                return false;
            }
            for (UINT i = 0; i < MAILBOX_OUT_SLOTS; ++i)
            {
                if (outputs[i].frame > newestFrame && outputs[i].publishValue && f1 >= outputs[i].publishValue)
                {
                    newestFrame = outputs[i].frame;
                    newestSlot = static_cast<int>(i);
                }
            }
            for (UINT i = 0; i < MAILBOX_OUT_SLOTS; ++i)
            {
                auto& output = outputs[i];
                if (!output.frame || static_cast<int>(i) == newestSlot || output.frame >= newestFrame || f1 < output.publishValue)
                {
                    continue;
                }
                // Latch once. A future GPU0 signal includes every receive recorded before this observation.
                if (!output.freeAfter)
                {
                    output.freeAfter = inputsReady.issuedValue() + 1;
                }
                if (f0 >= output.freeAfter)
                {
                    output = {};
                }
            }
            for (UINT i = 0; i < MailboxResources::READBACK_SLOTS; ++i)
            {
                auto& sample = samples[i];
                if (!sample.pending || f0 < sample.ready)
                {
                    continue;
                }
                uint32_t id = 0;
                std::memcpy(&id, resources.mappedReadback() + i * MailboxResources::READBACK_STRIDE, sizeof(id));
                const uint64_t sourceFrame = id;
                if (sample.frame > observations.displayedFrame)
                {
                    observations.displayedFrame = sample.frame;
                    observations.displayedSourceFrame = sourceFrame;
                }
                if (!id)
                {
                    ++observations.unavailableFrames;
                }
                else if (id > sample.frame)
                {
                    ++observations.badFrameIds;
                }
                else
                {
                    const uint64_t age = sample.frame - sourceFrame;
                    ++observations.ageHistogram[age < 7 ? age : 7];
                    ++observations.ageSamples;
                }
                sample.pending = false;
            }
            return true;
        }

        /** @brief Returns an input slot with no pending GPU1 readers, or -1. */
        int freeInput() const
        {
            const uint64_t completed = resultsReady.completedValue();
            for (UINT i = 0; i < MAILBOX_IN_SLOTS; ++i)
            {
                if (completed >= inputs[i].readDone)
                {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }
        /**
         * @brief Finds an output slot while keeping the latest published result available.
         * A retired slot is eligible when the new GPU1 input wait also covers its old GPU0 readers.
         * @param inputValue Reserved GPU0 input fence value for the new job.
         * @return Slot index, or -1 when none can be written safely.
         */
        int reusableOutput(uint64_t inputValue) const
        {
            if (!inputValue || inputValue > inputsReady.issuedValue())
            {
                return -1;
            }
            for (UINT i = 0; i < MAILBOX_OUT_SLOTS; ++i)
            {
                if (!outputs[i].frame)
                {
                    return static_cast<int>(i);
                }
            }
            int oldest = -1;
            for (UINT i = 0; i < MAILBOX_OUT_SLOTS; ++i)
            {
                const auto& output = outputs[i];
                if (static_cast<int>(i) != newestSlot && output.freeAfter && output.freeAfter <= inputValue &&
                    (oldest < 0 || output.frame < outputs[oldest].frame))
                {
                    oldest = static_cast<int>(i);
                }
            }
            return oldest;
        }
        UINT runnableJobs() const
        {
            const uint64_t f0 = inputsReady.completedValue(), f1 = resultsReady.completedValue();
            UINT count = 0;
            for (const auto& output : outputs)
            {
                if (output.frame && f0 >= output.inputValue && f1 < output.dataValue)
                {
                    ++count;
                }
            }
            return count;
        }
    };
}  // namespace nau::mgpu
