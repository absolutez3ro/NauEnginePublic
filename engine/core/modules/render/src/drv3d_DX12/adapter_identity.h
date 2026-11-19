// Copyright 2026 N-GINN LLC. All rights reserved.
#pragma once

#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <string>

namespace drv3d_dx12
{
struct PhysicalAdapterIdentity
{
  std::wstring pnpKey;
  bool typeKnown = false;
  bool virtualDisplay = false;
};

// LUIDs can alias the same physical board through indirect/remote display drivers.
// An unavailable identity remains unknown; PCI model IDs are never treated as unique.
inline PhysicalAdapterIdentity query_physical_adapter_identity(LUID luid)
{
  PhysicalAdapterIdentity result;
  const auto module = LoadLibraryExW(L"gdi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!module)
    return result;
  struct ModuleGuard
  {
    HMODULE value;
    ~ModuleGuard() { FreeLibrary(value); }
  } moduleGuard{module};
  const auto open = reinterpret_cast<PFND3DKMT_OPENADAPTERFROMLUID>(GetProcAddress(module, "D3DKMTOpenAdapterFromLuid"));
  const auto query = reinterpret_cast<PFND3DKMT_QUERYADAPTERINFO>(GetProcAddress(module, "D3DKMTQueryAdapterInfo"));
  const auto close = reinterpret_cast<PFND3DKMT_CLOSEADAPTER>(GetProcAddress(module, "D3DKMTCloseAdapter"));
  if (!open || !query || !close)
    return result;
  D3DKMT_OPENADAPTERFROMLUID adapter{};
  adapter.AdapterLuid = luid;
  if (open(&adapter) < 0)
    return result;
  struct AdapterGuard
  {
    D3DKMT_HANDLE value;
    PFND3DKMT_CLOSEADAPTER close;
    ~AdapterGuard() { D3DKMT_CLOSEADAPTER adapter{value}; close(&adapter); }
  } adapterGuard{adapter.hAdapter, close};
  D3DKMT_ADAPTERTYPE type{};
  D3DKMT_QUERYADAPTERINFO info{adapter.hAdapter, KMTQAITYPE_ADAPTERTYPE, &type, sizeof(type)};
  if (query(&info) >= 0)
  {
    result.typeKnown = true;
    result.virtualDisplay = type.SoftwareDevice || type.IndirectDisplayDevice || !type.RenderSupported;
  }
  WCHAR key[1024]{};
  UINT length = static_cast<UINT>(sizeof(key) / sizeof(key[0]));
  D3DKMT_QUERY_PHYSICAL_ADAPTER_PNP_KEY pnp{};
  pnp.PnPKeyType = D3DKMT_PNP_KEY_HARDWARE;
  pnp.pDest = key;
  pnp.pCchDest = &length;
  info.Type = KMTQAITYPE_PHYSICALADAPTERPNPKEY;
  info.pPrivateDriverData = &pnp;
  info.PrivateDriverDataSize = sizeof(pnp);
  if (query(&info) >= 0 && length > 0 && length <= sizeof(key) / sizeof(key[0]) && key[0])
  {
    key[sizeof(key) / sizeof(key[0]) - 1] = L'\0';
    result.pnpKey = key;
  }
  return result;
}

inline bool same_physical_adapter(const PhysicalAdapterIdentity &left, const PhysicalAdapterIdentity &right)
{
  return !left.pnpKey.empty() && !right.pnpKey.empty() && _wcsicmp(left.pnpKey.c_str(), right.pnpKey.c_str()) == 0;
}
} // namespace drv3d_dx12
