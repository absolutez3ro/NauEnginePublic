# DX12 secondary GPU discovery

Set `dx12.multiGpu` to `true` in `samples/sceneBase/config/app.json` to create a second native D3D12 device. The default is `false`. Applications with their own Dagor settings provider can also set `multiGpu:b=yes` in the `dx12` DataBlock. An explicitly supplied application property takes precedence over the DataBlock value.

The primary adapter selection and render pipeline stay unchanged. With the flag disabled there is no additional adapter enumeration, device creation or capability probing; MG-11 logs the disabled fallback reason once during initialization. With the flag enabled, secondary selection reuses the normal sorted candidate list, excluding the primary LUID and software/WARP adapters. Explicit primary selection by LUID, monitor, or WARP uses a separate enumeration for the secondary candidates. Driver preferences and the configured D3D feature level still apply. A failed secondary creation tries the next candidate; exhausting the list logs a warning and continues rendering on GPU 0.

## Native access for MG-04 consumers

The declarations are in `nau/3d/dag_drv3d.h`:

```cpp
d3d::GpuId primary = d3d::PRIMARY_GPU;
d3d::GpuId secondary = d3d::SECONDARY_GPU;
bool available = d3d::has_secondary_gpu();
void* native = d3d::get_device(secondary);
```

For DX12, cast the returned pointer to `ID3D12Device*`. GPU 0 refers to the existing primary device; the original zero-argument `d3d::get_device()` remains available. GPU 1 returns `nullptr` when disabled or unavailable. Unknown ids and calls outside an initialized driver's lifetime also return `nullptr`. Consumers can select GPU 0 explicitly for fallback or treat a null result as a refusal; GPU 1 never silently aliases GPU 0.

These are borrowed pointers. Use them on the render thread, do not release them, and release consumer-owned resources before driver shutdown or device recovery. The secondary device is released before DXGI/debug/runtime teardown and recreated after successful primary recovery. This task creates no secondary queues, shared resources, transfers, fences, or double buffering; those remain for the later MG implementations.

## Capability log

With `multiGpu` enabled, `NAU_LOG_INFO` reports each device's name and LUID, the HRESULT from `D3D12_FEATURE_D3D12_OPTIONS`, `CrossAdapterRowMajorTextureSupported`, and `CrossNodeSharingTier`. Failed queries retain the HRESULT and clear the capability fields.

These values are reported independently of whether a second device exists. A false row-major flag permits cross-adapter texture copies but restricts direct view usage. Cross-node sharing describes nodes within one adapter and is not a prerequisite for two separate adapters. MG-11 therefore does not reject a pair solely because either value is zero. The transfer layer must check its concrete resource and fence requirements. See [Microsoft's D3D12_OPTIONS documentation](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_d3d12_options).

## MG-11 fallback

Call these APIs on the render thread:

```cpp
if (!d3d::is_multi_gpu_available())
{
  // Run the existing primary-GPU technique here.
  const char *reason = d3d::get_multi_gpu_fallback_reason();
}
```

The driver reports `disabled by config`, `no suitable secondary GPU`,
`secondary adapter enumeration failed`, `secondary device creation failed`,
`cross-adapter capability query failed`, `secondary device removed`, or
`secondary transfer failed or unsupported`. The log prefix is
`DX12: MGPU fallback to GPU 0:`. Reasons are latched and logged on transitions,
not every frame. Before initialization and after shutdown the reason is
`driver not initialized`.

Both `has_secondary_gpu()` and `get_device(SECONDARY_GPU)` now check availability;
after a fallback they return false/nullptr. `get_device(PRIMARY_GPU)` continues
to return the existing primary device. `update_screen()` also checks secondary
health each frame with `GetDeviceRemovedReason()`. Secondary failure does not
trigger primary recovery. The secondary COM object is retained until driver
teardown/recovery so existing borrowed pointers are not invalidated by a query.

The MG-04 implementation and techniques must check availability before submitting
secondary work and select their existing single-GPU passes when it becomes false.
If shared resource/fence/channel creation or a secondary submission fails, call:

```cpp
d3d::report_secondary_gpu_failure(static_cast<uint32_t>(hr));
```

Successful HRESULTs are ignored. A failed HRESULT disables subsequent secondary
access for this driver lifetime (or until explicit primary driver recovery).
Consumers must retire their own resources, handle already-submitted work and
device-removal errors, and avoid waiting forever on a removed device's fence.
The driver cannot reroute already-submitted technique commands automatically.
MG-06 has no transfer implementation yet, so these integration points are
documented here; no MG-04 channel calls are introduced by this patch.

`is_multi_gpu_available()` confirms discovery, capability query success and
device health. It does not guarantee that a particular transfer format or shared
resource will work. Unsupported channels must take the fallback path described
above. New APIs in this patch are implemented in the DX12 driver.

### MG-11 verification

The platform-independent `MultiGpuFallbackStatus` test exercises successful and
failed HRESULTs, each fallback state, and persistence of removal after a later
successful health probe. The existing Windows tests still cover real native
device ownership and creation failures.

On Windows, run SceneBaseSample with `dx12.multiGpu` false and then true on a
single-GPU machine. The scene should stay unchanged and the log should state
`disabled by config` or `no suitable secondary GPU` respectively. On two GPUs,
check true availability. For the runtime path, the transfer layer must inject a
failed HRESULT, verify availability becomes false, switch its technique to GPU 0,
and verify primary rendering continues. Physical device removal and recovery
also require Windows hardware testing.

The historical MG-06 acceptance below describes the original branch, before this
MG-11 patch; it is not a claim of MG-11 Windows verification.

## Verification

The native component tests can run independently of an engine build, using MSVC and the Windows SDK:

```powershell
cmake -S engine/core/modules/render/tests/test_multi_gpu -B build/multi_gpu_tests
cmake --build build/multi_gpu_tests --config Debug
ctest --test-dir build/multi_gpu_tests -C Debug --output-on-failure
```

They check distinct LUIDs, software/WARP rejection, description and creation failures, empty results, COM ownership, reset, destruction, and capability queries using a real WARP device. The physical-device tests also check shutdown with D3D12/DXGI debug diagnostics. They skip when two D3D12 hardware adapters or the debug layer are unavailable. WARP never counts as a second physical GPU.

For acceptance on a two-GPU machine:

1. Run SceneBaseSample with the flag absent or `false`; compare its log and rendered scene with the baseline.
2. Set the flag to `true`; verify GPU 0 and GPU 1 names and distinct LUIDs, plus both capability query results, while the scene continues rendering.
3. Check `get_device(0) == get_device()`, `has_secondary_gpu()`, a non-null `get_device(1)`, and a null result for an unknown id.
4. Close the sample with DX12 CPU validation enabled and check for secondary-device leaks; also test device recovery if the test setup supports it.
5. Run with only one hardware GPU; check the warning, `has_secondary_gpu() == false`, and continued primary rendering.

## Результаты приёмки — 02.10.2026

Проверка выполнена на Windows, MSVC 14.44 и Windows SDK 10.0.26100.0. Полная сборка штатного `SceneBaseSample` в Release завершилась успешно. Для сборки использован официальный DXC 1.8.2403.2; ошибка штатного ISPC-скрипта при пробелах в пути обойдена прямым запуском комплектного ISPC. Эти действия не меняют исходники сторонних библиотек.

Все 8 компонентных тестов прошли без пропусков. Созданы два физических D3D12-устройства с разными LUID; проверены ошибки создания, владение COM, повторный reset, уничтожение и отсутствие оставшихся устройств/адаптеров/фабрик в аппаратном тесте с debug layer.

В `SceneBaseSample` основной адаптер выбирается штатным алгоритмом:

| GPU id | Адаптер | LUID HighPart:LowPart | HRESULT D3D12_OPTIONS | Row-major cross-adapter | CrossNodeSharingTier |
| --- | --- | --- | --- | --- | --- |
| 0 | NVIDIA GeForce RTX 2060 SUPER | 0:160506407 | S_OK | false | 0 |
| 1 | Intel(R) UHD Graphics | 0:75290 | S_OK | true | 0 |

Проверочный вариант приложения собран из временных копий исходников в `build/acceptance_sources`, с включённым CPU debug layer. Он загружает штатную сцену, проверяет API из потока рендера, сохраняет кадр и вызывает штатное завершение приложения. Проверочный код не включён в рабочие исходники или PR.

| Настройка | `has_secondary_gpu()` | GPU 0 совпадает с `get_device()` | Неизвестный id возвращает nullptr | Новые строки о GPU в логе | Код завершения |
| --- | --- | --- | --- | --- | --- |
| Флаг отсутствует | false | да | да | 0 | 0 |
| `multiGpu: false` | false | да | да | 0 | 0 |
| `multiGpu: true` | true | да | да | 4 | 0 |

При `true` лог содержит оба имени, разные LUID и результаты обеих проверок возможностей. Debug-очередь вторичного устройства пуста.

Во всех трёх запусках сохранён кадр 634×611. Кадры с отсутствующим флагом и с `true` совпали побайтово. При `false` отличаются 1400 пикселей в области панели отладки анимации; остальная сцена совпадает. Логи DX12 при отсутствующем флаге и при `false` отличаются только тремя значениями текущего потребления/резерва памяти (204.24 и 204.25 MiB).

После штатного завершения каждого запуска `is_inited()` возвращает false, оба новых accessor возвращают nullptr. `DXGI ReportLiveObjects` с `DETAIL | IGNORE_INTERNAL` завершился с S_OK и не добавил сообщений об оставшихся объектах.

Полную приёмку приложения без замечаний подтвердить нельзя: во всех трёх режимах основной рендер выдаёт одинаковые 50 ошибок D3D12 #1422 — использование неинициализированных render-target/depth-stencil ресурсов. Сообщения совпадают после удаления адресов объектов. Кроме того, при завершении не удаётся записать `cache/dx12.cache`. Эти сообщения воспроизводятся и при отсутствующем флаге; проверка не устанавливает их наличие в сборке до данного изменения.

Принудительная потеря/восстановление устройства и запуск сцены после физического отключения одного адаптера не проверялись. Отказы создания и исключение WARP проверены компонентными тестами. Измерение пропускной способности, создание общих ресурсов и межадаптерная передача относятся к следующим MG-задачам.

Локальные материалы проверки: `build/multi_gpu_tests/Testing/Temporary/LastTest.log`, `build/sample_build.log`, а также `build/acceptance/{absent,false,true}` с журналами, `probe.txt`, `shutdown.txt`, `result.json` и захваченными кадрами `frame.png`. Конфигурация после запусков восстановлена: `dx12.multiGpu` по умолчанию равен `false`.
