// multi_gpu_transfer_types.h
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once

#include <cstdint>

namespace d3d::mgpu
{
    /**
     * @brief Render-thread observer of the channel's native COPY lists.
     * attach is called once per side. endCopy resolves queries before Close; submitted follows ExecuteCommandLists.
     * cancel discards a list that was not executed. detach runs after both channel queues have drained.
     * source=true selects the sending side; ticket identifies the same transfer on both sides.
     */
    class ITransferProfiler
    {
    public:
        ITransferProfiler() = default;
        ITransferProfiler(const ITransferProfiler&) = delete;
        ITransferProfiler(ITransferProfiler&&) = delete;
        ITransferProfiler& operator=(const ITransferProfiler&) = delete;
        ITransferProfiler& operator=(ITransferProfiler&&) = delete;
        virtual ~ITransferProfiler() = default;

        virtual void attach(bool source, void* device, void* queue) = 0;
        virtual void beginCopy(bool source, void* list, uint64_t ticket) = 0;
        virtual void endCopy(bool source, void* list) = 0;
        virtual void submitted(bool source) = 0;
        virtual void cancel(bool source) = 0;
        virtual void detach() = 0;
    };
}  // namespace d3d::mgpu
