#include "multi_gpu_device.h"
#include "device_resource.h"
#include "nau/directx/d3d12sdklayers.h"

#include <dxgidebug.h>
#include <gtest/gtest.h>
#include <iostream>
#include <vector>
#include <wrl/implements.h>

using drv3d_dx12::CrossAdapterSupport;
using drv3d_dx12::is_secondary_gpu_candidate;
using drv3d_dx12::SecondaryGpuDevice;
using Microsoft::WRL::ComPtr;

namespace
{
class TestAdapter final : public Microsoft::WRL::RuntimeClass<
  Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDXGIAdapter1>
{
public:
  DXGI_ADAPTER_DESC1 info = {};
  HRESULT descriptionResult = S_OK;

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetParent(REFIID, void **) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnumOutputs(UINT, IDXGIOutput **) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID, LARGE_INTEGER *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1 *desc) override
  {
    *desc = info;
    return descriptionResult;
  }
};

ComPtr<TestAdapter> make_adapter()
{
  auto adapter = Microsoft::WRL::Make<TestAdapter>();
  adapter->info.AdapterLuid = {2, 3};
  adapter->info.VendorId = 0x10de;
  return adapter;
}

HRESULT WINAPI fail_create(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **)
{
  return DXGI_ERROR_UNSUPPORTED;
}

HRESULT WINAPI empty_create(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **result)
{
  *result = nullptr;
  return S_OK;
}

ComPtr<ID3D12Device> injectedDevice;
HRESULT WINAPI inject_create(IUnknown *, D3D_FEATURE_LEVEL, REFIID iid, void **result)
{
  return injectedDevice.CopyTo(iid, result);
}

    /**
     * @brief Verifies native resource ownership on the selected second physical GPU.
     */
    void checkSecondaryResources(ID3D12Device* secondaryDevice, ID3D12Device* primaryDevice)
    {
        drv3d_dx12::DeviceResource target;
        drv3d_dx12::DeviceResource buffer;
        ASSERT_HRESULT_SUCCEEDED(target.createRenderTarget(secondaryDevice, 64, 64));
        ASSERT_HRESULT_SUCCEEDED(buffer.createBuffer(secondaryDevice, 1024, D3D12_HEAP_TYPE_DEFAULT));
        for (const drv3d_dx12::DeviceResource* resource : {&target, &buffer})
        {
            ComPtr<ID3D12Device> owner;
            ASSERT_HRESULT_SUCCEEDED(resource->getResource()->GetDevice(IID_PPV_ARGS(&owner)));
            EXPECT_EQ(owner.Get(), secondaryDevice);
            EXPECT_NE(owner.Get(), primaryDevice);
        }
        // No commands were submitted; resources are released before the caller resets the device.
    }
}

TEST(MultiGpuSelection, RequiresDifferentLuidAndHardware)
{
  DXGI_ADAPTER_DESC1 candidate = {};
  const LUID primary = {12, 34};
  candidate.AdapterLuid = primary;
  EXPECT_FALSE(is_secondary_gpu_candidate(candidate, primary));
  candidate.AdapterLuid.HighPart++;
  EXPECT_TRUE(is_secondary_gpu_candidate(candidate, primary));
  candidate.AdapterLuid = primary;
  candidate.AdapterLuid.LowPart++;
  EXPECT_TRUE(is_secondary_gpu_candidate(candidate, primary));
  candidate.Flags = DXGI_ADAPTER_FLAG_SOFTWARE;
  EXPECT_FALSE(is_secondary_gpu_candidate(candidate, primary));
  candidate.Flags = 0;
  candidate.VendorId = 0x1414;
  candidate.DeviceId = 0x8c;
  EXPECT_FALSE(is_secondary_gpu_candidate(candidate, primary));
}

TEST(MultiGpuDevice, StartsEmptyAndResetIsIdempotent)
{
  SecondaryGpuDevice device;
  EXPECT_EQ(device.getDevice(), nullptr);
  device.reset();
  device.reset();
  EXPECT_EQ(device.getDevice(), nullptr);
  EXPECT_EQ(device.getCrossAdapterSupport().queryResult, E_FAIL);
}

TEST(MultiGpuDevice, RejectsInvalidInputAndDescriptionFailure)
{
  SecondaryGpuDevice device;
  auto adapter = make_adapter();
  EXPECT_EQ(device.init(nullptr, {}, D3D_FEATURE_LEVEL_11_0, fail_create), E_INVALIDARG);
  EXPECT_EQ(device.init(adapter.Get(), {}, D3D_FEATURE_LEVEL_11_0, nullptr), E_INVALIDARG);
  adapter->descriptionResult = E_FAIL;
  EXPECT_EQ(device.init(adapter.Get(), {}, D3D_FEATURE_LEVEL_11_0, fail_create), E_FAIL);
  EXPECT_EQ(device.getDevice(), nullptr);
}

TEST(MultiGpuDevice, RejectsPrimaryAndWarpBeforeDeviceCreation)
{
  SecondaryGpuDevice device;
  auto adapter = make_adapter();
  EXPECT_EQ(device.init(adapter.Get(), adapter->info.AdapterLuid, D3D_FEATURE_LEVEL_11_0, fail_create), E_INVALIDARG);
  adapter->info.Flags = DXGI_ADAPTER_FLAG_SOFTWARE;
  EXPECT_EQ(device.init(adapter.Get(), {}, D3D_FEATURE_LEVEL_11_0, fail_create), E_INVALIDARG);
  EXPECT_EQ(device.getDevice(), nullptr);
}

TEST(MultiGpuDevice, CreationFailureLeavesNoDeviceOrAdapterReference)
{
  SecondaryGpuDevice device;
  auto adapter = make_adapter();
  EXPECT_EQ(device.init(adapter.Get(), {}, D3D_FEATURE_LEVEL_11_0, fail_create), DXGI_ERROR_UNSUPPORTED);
  EXPECT_EQ(device.init(adapter.Get(), {}, D3D_FEATURE_LEVEL_11_0, empty_create), E_FAIL);
  EXPECT_EQ(device.getDevice(), nullptr);
  EXPECT_EQ(adapter->AddRef(), 2u);
  adapter->Release();
}

TEST(MultiGpuDevice, OwnsDeviceQueriesCapabilitiesAndReleasesOnResetAndFailure)
{
  ComPtr<IDXGIFactory4> factory;
  ASSERT_HRESULT_SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
  ComPtr<IDXGIAdapter1> warp;
  ASSERT_HRESULT_SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
  if (FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&injectedDevice))))
    GTEST_SKIP() << "D3D12 WARP unavailable";

  auto adapter = make_adapter();
  {
    SecondaryGpuDevice device;
    ASSERT_HRESULT_SUCCEEDED(device.init(adapter.Get(), {}, D3D_FEATURE_LEVEL_11_0, inject_create));
    EXPECT_EQ(device.getDevice(), injectedDevice.Get());
    EXPECT_EQ(adapter->AddRef(), 3u);
    adapter->Release();
    EXPECT_HRESULT_SUCCEEDED(device.getCrossAdapterSupport().queryResult);
    EXPECT_EQ(device.getDescription().AdapterLuid.LowPart, 2u);
    device.reset();
    EXPECT_EQ(device.getDevice(), nullptr);
    EXPECT_EQ(adapter->AddRef(), 2u);
    adapter->Release();
    ASSERT_HRESULT_SUCCEEDED(device.init(adapter.Get(), {}, D3D_FEATURE_LEVEL_11_0, inject_create));
    EXPECT_EQ(device.init(adapter.Get(), {}, D3D_FEATURE_LEVEL_11_0, fail_create), DXGI_ERROR_UNSUPPORTED);
    EXPECT_EQ(device.getDevice(), nullptr);
    ASSERT_HRESULT_SUCCEEDED(device.init(adapter.Get(), {}, D3D_FEATURE_LEVEL_11_0, inject_create));
  }
  EXPECT_EQ(adapter->AddRef(), 2u);
  adapter->Release();
  injectedDevice.Reset();
}

namespace
{
void check_hardware_pair()
{
  ComPtr<IDXGIFactory4> factory;
  ASSERT_HRESULT_SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
  ComPtr<ID3D12Device> primary;
  LUID primaryLuid = {};
  for (UINT index = 0;; ++index)
  {
    ComPtr<IDXGIAdapter1> adapter;
    const HRESULT hr = factory->EnumAdapters1(index, &adapter);
    if (hr == DXGI_ERROR_NOT_FOUND)
      break;
    ASSERT_HRESULT_SUCCEEDED(hr);
    DXGI_ADAPTER_DESC1 desc = {};
    ASSERT_HRESULT_SUCCEEDED(adapter->GetDesc1(&desc));
    if (!is_secondary_gpu_candidate(desc, primaryLuid))
      continue;
    if (!primary)
    {
      if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&primary))))
      {
        primaryLuid = desc.AdapterLuid;
        CrossAdapterSupport support;
        support.query(primary.Get());
        EXPECT_HRESULT_SUCCEEDED(support.queryResult);
        std::wcout << L"GPU 0: " << desc.Description << L", LUID=" << desc.AdapterLuid.HighPart << L":"
          << desc.AdapterLuid.LowPart << L", CrossAdapterRowMajorTextureSupported="
          << support.options.CrossAdapterRowMajorTextureSupported << L", CrossNodeSharingTier="
          << support.options.CrossNodeSharingTier << std::endl;
      }
      continue;
    }
    SecondaryGpuDevice secondary;
    if (FAILED(secondary.init(adapter.Get(), primaryLuid, D3D_FEATURE_LEVEL_11_0, D3D12CreateDevice)))
      continue;
    EXPECT_NE(secondary.getDevice(), primary.Get());
    EXPECT_HRESULT_SUCCEEDED(secondary.getCrossAdapterSupport().queryResult);
    const auto &support = secondary.getCrossAdapterSupport();
    std::wcout << L"GPU 1: " << desc.Description << L", LUID=" << desc.AdapterLuid.HighPart << L":"
      << desc.AdapterLuid.LowPart << L", CrossAdapterRowMajorTextureSupported="
      << support.options.CrossAdapterRowMajorTextureSupported << L", CrossNodeSharingTier="
      << support.options.CrossNodeSharingTier << std::endl;
    // Exercise the production resource component on the selected physical GPU 1.
    // WARP is excluded by the adapter selection above.
    ASSERT_NO_FATAL_FAILURE(checkSecondaryResources(secondary.getDevice(), primary.Get()));
    secondary.reset();
    EXPECT_EQ(secondary.getDevice(), nullptr);
    return;
  }
  GTEST_SKIP() << "Two physical D3D12 adapters are required";
}
}

TEST(MultiGpuHardware, CreatesSecondPhysicalDeviceWhenAvailable)
{
  check_hardware_pair();
}

TEST(MultiGpuHardware, DebugLayerShutdown)
{
  ComPtr<ID3D12Debug> debug;
  if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
    GTEST_SKIP() << "D3D12 debug layer unavailable";
  debug->EnableDebugLayer();

  ComPtr<IDXGIInfoQueue> queue;
  ComPtr<IDXGIDebug1> dxgiDebug;
  if (FAILED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&queue))) ||
      FAILED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgiDebug))))
    GTEST_SKIP() << "DXGI debug diagnostics unavailable";
  queue->ClearStoredMessages(DXGI_DEBUG_ALL);
  check_hardware_pair();
  if (::testing::Test::HasFailure() || ::testing::Test::IsSkipped())
    return;
  ASSERT_HRESULT_SUCCEEDED(dxgiDebug->ReportLiveObjects(DXGI_DEBUG_ALL,
    static_cast<DXGI_DEBUG_RLO_FLAGS>(DXGI_DEBUG_RLO_DETAIL | DXGI_DEBUG_RLO_IGNORE_INTERNAL)));
  for (UINT64 index = 0; index < queue->GetNumStoredMessages(DXGI_DEBUG_ALL); ++index)
  {
    SIZE_T size = 0;
    ASSERT_HRESULT_SUCCEEDED(queue->GetMessage(DXGI_DEBUG_ALL, index, nullptr, &size));
    std::vector<char> storage(size);
    auto *message = reinterpret_cast<DXGI_INFO_QUEUE_MESSAGE *>(storage.data());
    ASSERT_HRESULT_SUCCEEDED(queue->GetMessage(DXGI_DEBUG_ALL, index, message, &size));
    EXPECT_NE(message->Severity, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR) << message->pDescription;
    EXPECT_NE(message->Severity, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION) << message->pDescription;
    const std::string description(message->pDescription, message->DescriptionByteLength);
    EXPECT_EQ(description.find("Live IDXGIAdapter"), std::string::npos) << description;
    EXPECT_EQ(description.find("Live IDXGIFactory"), std::string::npos) << description;
    EXPECT_EQ(description.find("Live ID3D12Device"), std::string::npos) << description;
  }
}
