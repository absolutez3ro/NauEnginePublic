#include "nau/mgpu/Common/pch.h"
#include "nau/mgpu/Common/CrossAdapterParticleEmitter.h"


#include "nau/mgpu/Utils/MathHelper.h"
#include "nau/mgpu/Common/ParticleEmitter.h"

void CrossAdapterParticleEmitter::InitPSO(const std::shared_ptr<GDevice>& otherDevice)
{
    CD3DX12_DESCRIPTOR_RANGE rng[4];
    rng[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
    rng[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1);
    rng[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 2);
    rng[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 3);

    computeRS = std::make_shared<GRootSignature>();
    computeRS->AddConstantParameter(sizeof(EmitterData) / sizeof(float), 0); // EmitterData		
    computeRS->AddDescriptorParameter(&rng[0], 1);
    computeRS->AddDescriptorParameter(&rng[1], 1);
    computeRS->AddDescriptorParameter(&rng[2], 1);
    computeRS->AddDescriptorParameter(&rng[3], 1);
        computeRS->Initialize(otherDevice, false, D3D12_ROOT_SIGNATURE_FLAG_NONE);
}

void CrossAdapterParticleEmitter::CreateBuffers()
{
    if (ParticlesPool)
    {
        ParticlesPool->Reset();
        ParticlesPool.reset();
    }

    if (InjectedParticles)
    {
        InjectedParticles->Reset();
        InjectedParticles.reset();
    }

    if (ParticlesAlive)
    {
        ParticlesAlive->Reset();
        ParticlesAlive.reset();
    }

    if (ParticlesDead)
    {
        ParticlesDead->Reset();
        ParticlesDead.reset();
    }

    if (CrossAdapterAliveIndexes)
    {
        CrossAdapterAliveIndexes->Reset();
        CrossAdapterAliveIndexes.reset();
    }

    if (CrossAdapterDeadIndexes)
    {
        CrossAdapterDeadIndexes->Reset();
        CrossAdapterDeadIndexes.reset();
    }

    if (CrossAdapterParticles)
    {
        CrossAdapterParticles->Reset();
        CrossAdapterParticles.reset();
    }

    ParticlesPool = std::make_shared<GBuffer>(secondDevice, static_cast<UINT>(sizeof(ParticleData)),
                                              primeParticleEmitter->emitterData.ParticlesTotalCount,
                                              L"Second Particles Pool Buffer",
                                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    InjectedParticles = std::make_shared<GBuffer>(secondDevice, static_cast<UINT>(sizeof(ParticleData)),
                                                  primeParticleEmitter->emitterData.ParticleInjectCount,
                                                  L"Second Injected Particle Buffer",
                                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ParticlesAlive = std::make_shared<CounteredStructBuffer<DWORD>>(secondDevice,
                                                                    primeParticleEmitter->emitterData.
                                                                    ParticlesTotalCount,
                                                                    L"Second Particles Alive Index Buffer");
    ParticlesDead = std::make_shared<CounteredStructBuffer<DWORD>>(secondDevice,
                                                                   primeParticleEmitter->emitterData.
                                                                   ParticlesTotalCount,
                                                                   L"Second Particles Dead Index Buffer");

    auto desc = ParticlesAlive->GetD3D12ResourceDesc();
    CrossAdapterAliveIndexes = std::make_shared<GCrossAdapterResource>(desc, device, secondDevice,
                                                                       L"Cross Adapter Particle Alive Index Buffer");
    CrossAdapterDeadIndexes = std::make_shared<GCrossAdapterResource>(desc, device, secondDevice,
                                                                      L"Cross Adapter Particle Dead Index Buffer");

    desc = ParticlesPool->GetD3D12ResourceDesc();
    CrossAdapterParticles = std::make_shared<GCrossAdapterResource>(desc, device, secondDevice,
                                                                    L"Cross Adapter Particle Buffer");


    newParticles.resize(InjectedParticles->GetElementCount());

    {
        std::vector<UINT> deadIndex;

        for (DWORD i = 0; i < primeParticleEmitter->emitterData.ParticlesTotalCount; ++i)
        {
            deadIndex.push_back(i);
        }

        auto queue = secondDevice->GetCommandQueue();
        auto cmdList = queue->GetCommandList();

        ParticlesDead->LoadData(deadIndex.data(), cmdList);
        ParticlesDead->SetCounterValue(cmdList, primeParticleEmitter->emitterData.ParticlesTotalCount);

        cmdList->TransitionBarrier(ParticlesDead->GetD3D12Resource(), D3D12_RESOURCE_STATE_COMMON);
        cmdList->FlushResourceBarriers();

        queue->ExecuteCommandList(cmdList);
        queue->Flush();
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc;
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = ParticlesPool->GetElementCount();
    uavDesc.Buffer.StructureByteStride = ParticlesPool->GetStride();
    uavDesc.Buffer.CounterOffsetInBytes = 0;

    ParticlesPool->CreateUnorderedAccessView(&uavDesc, &updateDescriptors, 0);

    uavDesc.Buffer.NumElements = ParticlesDead->GetElementCount();
    uavDesc.Buffer.StructureByteStride = ParticlesDead->GetStride();
    uavDesc.Buffer.CounterOffsetInBytes = ParticlesDead->GetBufferSize() - sizeof(DWORD);
    ParticlesDead->CreateUnorderedAccessView(&uavDesc, &updateDescriptors, 1, ParticlesDead->GetD3D12Resource());
    ParticlesAlive->CreateUnorderedAccessView(&uavDesc, &updateDescriptors, 2, ParticlesAlive->GetD3D12Resource());

    uavDesc.Buffer.NumElements = InjectedParticles->GetElementCount();
    uavDesc.Buffer.StructureByteStride = InjectedParticles->GetStride();
    uavDesc.Buffer.CounterOffsetInBytes = 0;
    InjectedParticles->CreateUnorderedAccessView(&uavDesc, &updateDescriptors, 3);
}

CrossAdapterParticleEmitter::CrossAdapterParticleEmitter(std::shared_ptr<GDevice> primeDevice,
                                                         const std::shared_ptr<GDevice>& otherDevice,
                                                         DWORD particleCount): secondDevice(otherDevice)
{
    this->device = primeDevice;


    InitPSO(otherDevice);


    updateDescriptors = secondDevice->AllocateDescriptors(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4);

    primeParticleEmitter = std::make_shared<ParticleEmitter>(primeDevice, particleCount);


    CreateBuffers();
}

void CrossAdapterParticleEmitter::Update()
{
    primeParticleEmitter->Update();
}

void CrossAdapterParticleEmitter::Draw(const std::shared_ptr<GCommandList>& cmdList)
{
    if (UseSharedCompute)
    {
        cmdList->CopyResource(primeParticleEmitter->ParticlesPool->GetD3D12Resource(),
                              CrossAdapterParticles->GetPrimeResource().GetD3D12Resource());
        cmdList->CopyResource(primeParticleEmitter->ParticlesAlive->GetD3D12Resource(),
                              CrossAdapterAliveIndexes->GetPrimeResource().GetD3D12Resource());
    }

    primeParticleEmitter->Draw(cmdList);
}

void CrossAdapterParticleEmitter::Dispatch(const std::shared_ptr<GCommandList>& cmdList)
{
    if (DirtyActivated == Enable)
    {
        if (primeParticleEmitter->isWorked)
        {
            primeParticleEmitter->ParticlesAlive->ReadCounter(&primeParticleEmitter->emitterData.ParticlesAliveCount);

            cmdList->CopyResource(ParticlesPool->GetD3D12Resource(),
                                  CrossAdapterParticles->GetSharedResource().GetD3D12Resource());
            cmdList->CopyResource(ParticlesAlive->GetD3D12Resource(),
                                  CrossAdapterAliveIndexes->GetSharedResource().GetD3D12Resource());
            cmdList->CopyResource(ParticlesDead->GetD3D12Resource(),
                                  CrossAdapterDeadIndexes->GetSharedResource().GetD3D12Resource());
        }

        DirtyActivated = None;
    }

    if (DirtyActivated == Disable)
    {
        cmdList->CopyResource(primeParticleEmitter->ParticlesPool->GetD3D12Resource(),
                              CrossAdapterParticles->GetPrimeResource().GetD3D12Resource());
        cmdList->CopyResource(primeParticleEmitter->ParticlesAlive->GetD3D12Resource(),
                              CrossAdapterAliveIndexes->GetPrimeResource().GetD3D12Resource());
        cmdList->CopyResource(primeParticleEmitter->ParticlesDead->GetD3D12Resource(),
                              CrossAdapterDeadIndexes->GetPrimeResource().GetD3D12Resource());

        DirtyActivated = None;
    }


    if (UseSharedCompute)
    {
        ParticlesAlive->ReadCounter(&primeParticleEmitter->emitterData.ParticlesAliveCount);

        cmdList->TransitionBarrier(ParticlesPool->GetD3D12Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->TransitionBarrier(ParticlesAlive->GetD3D12Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->TransitionBarrier(ParticlesDead->GetD3D12Resource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->FlushResourceBarriers();

        ParticlesAlive->CopyCounterForRead(cmdList);

        cmdList->CopyResource(CrossAdapterParticles->GetSharedResource().GetD3D12Resource(),
                              ParticlesPool->GetD3D12Resource());
        cmdList->CopyResource(CrossAdapterAliveIndexes->GetSharedResource().GetD3D12Resource(),
                              ParticlesAlive->GetD3D12Resource());
        cmdList->CopyResource(CrossAdapterDeadIndexes->GetSharedResource().GetD3D12Resource(),
                              ParticlesDead->GetD3D12Resource());
    }
    else
    {
        primeParticleEmitter->Dispatch(cmdList);

        cmdList->CopyResource(CrossAdapterParticles->GetPrimeResource().GetD3D12Resource(),
                              primeParticleEmitter->ParticlesPool->GetD3D12Resource());
        cmdList->CopyResource(CrossAdapterAliveIndexes->GetPrimeResource().GetD3D12Resource(),
                              primeParticleEmitter->ParticlesAlive->GetD3D12Resource());
        cmdList->CopyResource(CrossAdapterDeadIndexes->GetPrimeResource().GetD3D12Resource(),
                              primeParticleEmitter->ParticlesDead->GetD3D12Resource());
    }
}

void CrossAdapterParticleEmitter::ChangeParticleCount(const UINT count)
{
    primeParticleEmitter->ChangeParticleCount(count);

    CreateBuffers();
}

void CrossAdapterParticleEmitter::EnableShared()
{
    primeParticleEmitter->ParticlesAlive->ReadCounter(&primeParticleEmitter->emitterData.ParticlesAliveCount);

    UseSharedCompute = true;

    DirtyActivated = Enable;
}

void CrossAdapterParticleEmitter::DisableShared()
{
    UseSharedCompute = false;

    DirtyActivated = Disable;
}
