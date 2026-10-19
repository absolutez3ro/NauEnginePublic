#include "nau/mgpu/Utils/d3dUtil.h"
#include "nau/mgpu/Utils/ConcurrentQueue.h"
#include "nau/mgpu/Utils/GameTimer.h"
#include "nau/mgpu/Utils/IDGenerator.hpp"
#include "nau/mgpu/Utils/Lazy.h"
#include "nau/mgpu/Utils/LockThreadQueue.h"
#include "nau/mgpu/Utils/MathHelper.h"
#include "nau/mgpu/Allocator/MemoryAllocator.h"
#include "nau/mgpu/Common/pch.h"
#include "nau/mgpu/Graphics/GDeviceFactory.h"
#include "nau/mgpu/Graphics/GDevice.h"
#include "nau/mgpu/Graphics/GShader.h"
#include "nau/mgpu/Graphics/GCommandQueue.h"
#include "nau/mgpu/Graphics/GCommandList.h"
#include "nau/mgpu/Graphics/GResource.h"
#include "nau/mgpu/Graphics/GBuffer.h"
#include "nau/mgpu/Graphics/DirectXBuffers.h"
#include "nau/mgpu/Graphics/GTexture.h"
#include "nau/mgpu/Graphics/GMeshBuffer.h"
#include "nau/mgpu/Graphics/GRenderTarger.h"
#include "nau/mgpu/Graphics/GResourceStateTracker.h"
#include "nau/mgpu/Graphics/GAllocator.h"
#include "nau/mgpu/Graphics/GDescriptor.h"
#include "nau/mgpu/Graphics/GDescriptorHeap.h"
#include "nau/mgpu/Graphics/GDataUploader.h"
#include "nau/mgpu/Graphics/GRootSignature.h"
#include "nau/mgpu/Graphics/PSO.h"
#include "nau/mgpu/Graphics/GraphicPSO.h"
#include "nau/mgpu/Graphics/ComputePSO.h"

namespace nau::PEPEngine
{
    static_assert(Utils::globalCountFrameResources == 3);
    static_assert(Graphics::GraphicAdapterCount == 3);
    static_assert(static_cast<int>(Graphics::GQueueType::Count) == 3);
}
