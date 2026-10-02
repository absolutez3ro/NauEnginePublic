#include "multi_gpu_device.h"

#include <gtest/gtest.h>
#include <iostream>
#include <wrl/implements.h>

using namespace drv3d_dx12;
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
} // namespace

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

TEST(MultiGpuHardware, CreatesSecondPhysicalDeviceWhenAvailable)
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
        std::wcout << L"GPU 0: " << desc.Description << std::endl;
      }
      continue;
    }
    SecondaryGpuDevice secondary;
    if (FAILED(secondary.init(adapter.Get(), primaryLuid, D3D_FEATURE_LEVEL_11_0, D3D12CreateDevice)))
      continue;
    EXPECT_NE(secondary.getDevice(), primary.Get());
    EXPECT_HRESULT_SUCCEEDED(secondary.getCrossAdapterSupport().queryResult);
    std::wcout << L"GPU 1: " << desc.Description << std::endl;
    secondary.reset();
    EXPECT_EQ(secondary.getDevice(), nullptr);
    return;
  }
  GTEST_SKIP() << "Two physical D3D12 adapters are required";
}
