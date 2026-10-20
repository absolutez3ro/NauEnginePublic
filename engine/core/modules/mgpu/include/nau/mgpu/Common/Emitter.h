#pragma once
#include "d3d12.h"
#include "SimpleMath.h"
#include "nau/mgpu/Allocator/MemoryAllocator.h"
#include <memory>

#include "nau/mgpu/Graphics/GCommandList.h"
#include "nau/mgpu/Common/Material.h"

// Base Renderer -> Component from A is not transferred (B has no GameObject); its pure virtual Update/Draw are declared here.
class Emitter
{
protected:
    std::shared_ptr<GRootSignature> renderSignature;
    std::shared_ptr<GRootSignature> computeSignature;
    std::shared_ptr<GraphicPSO> renderPSO;
    std::shared_ptr<ComputePSO> injectedPSO;
    std::shared_ptr<ComputePSO> simulatedPSO;
    std::shared_ptr<GShader> injectedShader;
    std::shared_ptr<GShader> simulatedShader;

    void PSOInitialize();

    std::vector<std::shared_ptr<GTexture>> Atlas;

    std::shared_ptr<GDevice> device;

public:
    virtual void Update() = 0;
    virtual void Draw(const std::shared_ptr<GCommandList>& cmdList) = 0;
    virtual void Dispatch(const std::shared_ptr<GCommandList>& cmdList) = 0;
};
