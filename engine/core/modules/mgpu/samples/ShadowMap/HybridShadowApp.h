#pragma once

#include "nau/mgpu/Common/AssetsLoader.h"
#include "nau/mgpu/Common/d3dApp.h"
#include "nau/mgpu/Graphics/GDeviceFactory.h"
#include "nau/mgpu/Common/GModel.h"
#include "nau/mgpu/Graphics/GraphicPSO.h"
#include "nau/mgpu/Common/Light.h"
#include "SceneRenderer.h"
#include "nau/mgpu/Common/RenderModeFactory.h"
#include "nau/mgpu/Common/ShadowMap.h"
#include "FrameResource.h"
#include "nau/mgpu/Common/SSAO.h"
#include "nau/mgpu/Common/GCrossAdapterResource.h"

using namespace DirectX::SimpleMath;
using namespace nau::PEPEngine;
using namespace Graphics;


class HybridShadowApp :
    public Common::D3DApp
{
public:
    HybridShadowApp(HINSTANCE hInstance);

    ~HybridShadowApp() override;;

    bool Initialize() override;


    int Run() override;

protected:
    void inline Update(const GameTimer& gt) override;
    void inline UpdateMaterials();
    void inline UpdateShadowTransform(const GameTimer& gt);
    void inline UpdateShadowPassCB(const GameTimer& gt);
    void inline UpdateMainPassCB(const GameTimer& gt);
    void inline UpdateSsaoCB(const GameTimer& gt);
    void inline PopulateShadowMapCommands(GraphicsAdapter adapter, std::shared_ptr<GCommandList> cmdList);
    void inline PopulateNormalMapCommands(const std::shared_ptr<GCommandList>& cmdList);
    void inline PopulateAmbientMapCommands(const std::shared_ptr<GCommandList>& cmdList);
    void inline PopulateForwardPathCommands(const std::shared_ptr<GCommandList>& cmdList);
    void inline PopulateDrawCommands(GraphicsAdapter adapterIndex, const std::shared_ptr<GCommandList>& cmdList,
                                     RenderMode type);
    void inline PopulateCopyResource(const std::shared_ptr<GCommandList>& cmdList, const GResource& srcResource,
                                     const GResource& dstResource);

    void Draw(const GameTimer& gt) override;

    void OnResize() override;

    bool InitMainWindow() override;;

private:
    void Flush() override;;
    void inline InitDevices();
    void inline InitFrameResource();
    void inline InitRootSignature();
    void inline InitPipeLineResource();
    void inline CreateMaterials();
    void inline InitSRVMemoryAndMaterials();
    void inline InitRenderPaths();
    void inline BuildForwardDepthMap();
    void inline LoadStudyTexture();
    void inline LoadModels();
    void inline MipMasGenerate();
    void inline DublicateResource();
    void inline SortGO();
    std::shared_ptr<SceneRenderer> inline CreateRenderer(UINT deviceIndex, std::shared_ptr<GModel> model,
                                                         const Matrix& world);
    void inline AddMultiDeviceOpaqueRenderComponent(const Matrix& world, const std::wstring& modelName,
                                                    RenderMode psoType = RenderMode::Opaque);
    void inline CreateGO();

    void CalculateFrameStats() override;
    void LogWriting();

    UINT64 gpuTimes[GraphicAdapterCount];
    std::atomic<bool> logThreadIsAlive = true;
    LockThreadQueue<std::wstring> logQueue{};
    bool finishTest = false;

    std::atomic<bool> UseOnlyPrime = true;

    D3D12_VIEWPORT fullViewport{};
    D3D12_RECT fullRect;

    std::vector<std::shared_ptr<GDevice>> devices = std::vector<std::shared_ptr<GDevice>>();

    std::vector<GDescriptor> srvTexturesMemory = std::vector<GDescriptor>();

    std::vector<AssetsLoader> assets = std::vector<AssetsLoader>();

    std::vector<std::unordered_map<std::wstring, std::shared_ptr<GModel>>> models = std::vector<
        std::unordered_map<std::wstring, std::shared_ptr<GModel>>>();

    std::shared_ptr<GRootSignature> primeDeviceSignature;
    std::shared_ptr<GRootSignature> ssaoPrimeRootSignature;
    std::shared_ptr<GRootSignature> secondDeviceShadowMapSignature;

    RenderModeFactory defaultPrimePipelineResources;

    std::shared_ptr<GraphicPSO> shadowMapPSOSecondDevice;
    std::shared_ptr<GCrossAdapterResource> crossAdapterShadowMap;
    GTexture primeCopyShadowMap;
    GDescriptor primeCopyShadowMapSRV;
    std::shared_ptr<ShadowMap> shadowPathSecondDevice;

    std::shared_ptr<ShadowMap> shadowPathPrimeDevice;


    std::vector<D3D12_INPUT_ELEMENT_DESC> defaultInputLayout{};


    std::shared_ptr<SSAO> ambientPrimePath;
    // B: depth buffer of the forward pass (in A it belonged to SSAA, which is not transferred).
    GTexture forwardDepthMap;
    GDescriptor forwardDepthMapDsv;

    // B: owners of the lights and the camera rig of A's scene (platform -> rotater -> MainCamera);
    // GameObject, Transform and Rotater are not transferred.
    std::vector<std::shared_ptr<Light>> sceneLights = std::vector<std::shared_ptr<Light>>();
    Matrix platformWorld = Matrix::Identity;
    Vector3 rotaterPosition = Vector3::Zero;
    Vector3 rotaterEulerAngles = Vector3::Zero;
    Matrix cameraLocal = Matrix::Identity;
    Matrix cameraWorld = Matrix::Identity;

    std::vector<std::vector<std::vector<std::shared_ptr<SceneRenderer>>>> typedRenderer = std::vector
        <std::vector<std::vector<std::shared_ptr<SceneRenderer>>>>();

    PassConstants mainPassCB;
    PassConstants shadowPassCB;


    ComPtr<ID3D12Fence> primeFence;
    ComPtr<ID3D12Fence> secondFence;
    UINT64 sharedFenceValue = 0;

    std::vector<std::shared_ptr<FrameResource>> frameResources = std::vector<std::shared_ptr<
        FrameResource>>();
    std::shared_ptr<FrameResource> currentFrameResource = nullptr;
    std::atomic<UINT> currentFrameResourceIndex = 0;

    std::vector<Light*> lights = std::vector<Light*>();

    float mLightNearZ = 0.0f;
    float mLightFarZ = 0.0f;
    Vector3 mLightPosW;
    Matrix mLightView = Matrix::Identity;
    Matrix mLightProj = Matrix::Identity;
    Matrix mShadowTransform = Matrix::Identity;

    float mLightRotationAngle = 0.0f;
    Vector3 mBaseLightDirections[3] = {
        Vector3(0.57735f, -0.57735f, 0.57735f),
        Vector3(-0.57735f, -0.57735f, 0.57735f),
        Vector3(0.0f, -0.707f, -0.707f)
    };
    Vector3 mRotatedLightDirections[3];


    DirectX::BoundingSphere mSceneBounds;


    UINT pathMapShow = 0;
    //off, shadowMap, ssaoMap
    const UINT maxPathMap = 3;
    LRESULT MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) override;
};
