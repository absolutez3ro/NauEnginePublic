#pragma once

#include "nau/mgpu/Graphics/GDescriptor.h"
#include "nau/mgpu/Common/ShaderBuffersData.h"
#include "nau/mgpu/Graphics/GTexture.h"

using namespace nau::PEPEngine;
using namespace Graphics;


struct FrameResource
{
    FrameResource(std::shared_ptr<GDevice> primeDevices, std::shared_ptr<GDevice> secondDevice, UINT passCount,
                  UINT materialCount);
    FrameResource(const FrameResource& rhs) = delete;
    FrameResource& operator=(const FrameResource& rhs) = delete;
    ~FrameResource();


    GDescriptor BackBufferRTVMemory;

    std::shared_ptr<ConstantUploadBuffer<PassConstants>> PrimePassConstantUploadBuffer;
    std::shared_ptr<ConstantUploadBuffer<PassConstants>> ShadowPassConstantUploadBuffer;
    std::shared_ptr<ConstantUploadBuffer<SsaoConstants>> SsaoConstantUploadBuffer;

    std::vector<std::shared_ptr<StructuredUploadBuffer<MaterialConstants>>> MaterialBuffers =
        std::vector<std::shared_ptr<StructuredUploadBuffer<MaterialConstants>>>();


    UINT64 PrimeRenderFenceValue = 0;
    UINT64 PrimeCopyFenceValue = 0;
    UINT64 SecondRenderFenceValue = 0;
};
