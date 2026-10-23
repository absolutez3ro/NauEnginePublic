# MG-15: гайд по MGPU для команд AO, Shadow и DOF

Гайд описывает API ветки `mgpu/AO` поверх командной интеграции MG-12. Общая цепочка: GPU 0 готовит входы → каналы MG-08/09 передают их на GPU 1 → техника считает результат → канал возвращает его на GPU 0. Рабочий пример техники — [SSAO](ambient_occlusion.md), минимальный пример передачи — [демка MG-12](mgpu_mg12_sample_2026_10_23.md).

## 1. Как проверить, что GPU 1 доступен

В `samples/sceneBase/config/app.json` включите `dx12.multiGpu`. Для двух аппаратных адаптеров оставьте `dx12.secondaryWarpForTesting=false`; для проверки с одним аппаратным GPU задайте `true`, чтобы вторым устройством стал WARP. Меняйте существующие поля конфигурации, сохраняя остальные настройки приложения.

После инициализации драйвера, на render thread:

```cpp
if (!d3d::is_multi_gpu_available())
{
    NAU_LOG_INFO("MGPU fallback: {}", d3d::get_multi_gpu_fallback_reason());
    // Выполнить вариант техники на GPU 0.
}
```

`has_secondary_gpu()` сообщает о существовании устройства, а `is_multi_gpu_available()` учитывает его здоровье и fallback MG-11. Даже `true` ещё не гарантирует поддержку нужного формата или создания канала: проверяйте результаты инициализации своей техники. Ошибку с неуспешным HRESULT передавайте в `d3d::report_secondary_gpu_failure(...)`; занятый слот сам по себе не означает отказ устройства.

`d3d::get_device(d3d::SECONDARY_GPU)` возвращает заимствованный `ID3D12Device*`; вызывать для него `Release()` нельзя. WARP явно помечается `TEST` в заголовке демки. Проверка с WARP подтверждает работоспособность, но не ускорение на аппаратной паре.

## 2. Как создать ресурс на GPU 1

Graphics использует публичные мосты Render: [`multi_gpu_ssao.h`](../engine/core/modules/render/include/nau/3d/multi_gpu_ssao.h) или [`multi_gpu_fullscreen.h`](../engine/core/modules/render/include/nau/3d/multi_gpu_fullscreen.h). Для новой техники нативные объекты создаются внутри DX12 backend, как в [`multi_gpu_ssao.cpp`](../engine/core/modules/render/src/drv3d_DX12/multi_gpu_ssao.cpp) и [`device_ssao_pass.cpp`](../engine/core/modules/render/src/drv3d_DX12/device_ssao_pass.cpp).

Память выделяет `NauDeviceResourceAllocator` MG-10 через пул Nau. Ниже — фрагменты полей и инициализации владельца ресурсов в Render; `width` и `height` — размер входного кадра:

```cpp
eastl::shared_ptr<drv3d_dx12::NauDeviceResourceAllocator> m_allocator;
drv3d_dx12::DeviceResource m_normals;
drv3d_dx12::DeviceResource m_result;
```

```cpp
m_allocator = eastl::make_shared<drv3d_dx12::NauDeviceResourceAllocator>();
HRESULT result = m_allocator->init(
    static_cast<ID3D12Device*>(d3d::get_device(d3d::SECONDARY_GPU)));
if (SUCCEEDED(result))
{
    result = m_normals.createTexture(m_allocator.get(), width, height, DXGI_FORMAT_R16G16_FLOAT);
}
if (SUCCEEDED(result))
{
    result = m_result.createTexture(m_allocator.get(), width, height, DXGI_FORMAT_R8_UNORM,
                                    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
}
```

Аллокатор должен жить дольше ресурсов, ресурсы — дольше использующей их GPU-работы. Новый placed render target перед первым draw инициализируется clear/discard или полной копией. Очередь, root signature, PSO и descriptor heaps создаются на устройстве GPU 1; объекты GPU 0 туда не передаются. Байткод VS/PS можно брать из общего shader cache, как в MG-12/AO. Shared heap межадаптерной передачи создаёт канал MG-08; своей технике отдельный транспорт не нужен.

## 3. Как передать входы и вернуть результат

Контракт находится в [`dag_drv3d_multi_gpu.h`](../engine/core/modules/render/include/nau/3d/dag_drv3d_multi_gpu.h). Создайте каналы один раз через `create_transfer_channel(TransferChannelDesc)`, проверив возвращённый handle. Для каждого канала задайте направление, размер и формат; для буфера — `payload=Buffer` и `byteSize`.

| Направление | Отправка | Приём |
| --- | --- | --- |
| GPU 0 → GPU 1 | `send_from_primary(channel, engineSource)` → ticket | `receive_on_secondary(channel, ticket, nativeDestination, destinationFree)` → ready fence/value |
| GPU 1 → GPU 0 | `send_from_secondary(channel, nativeSource, sourceReady)` → ticket и sourceReleased | `receive_on_primary(channel, ticket, engineDestination)` → bool |

Порядок работы AO показывает [`renderSsao`](../engine/core/modules/render/src/drv3d_DX12/multi_gpu_ssao.cpp):

1. GPU 0 отправляет normals и depth в два входных канала.
2. COPY-очереди GPU 1 ждут `destinationFree` — последнего использования входов предыдущим проходом, затем заполняют native-текстуры.
3. DIRECT-очередь GPU 1 ждёт оба ready fence/value, исполняет SSAO и два blur, сигнализирует готовность результата.
4. Выходной канал ждёт эту готовность, копирует результат в shared-слот и возвращает `sourceReleased`. Следующая перезапись результата на GPU 1 должна быть упорядочена после этой точки.
5. `receive_on_primary` ставит копию в текстуру GPU 0 и GPU-ожидание перед последующими командами. Resolve после этого вызова видит полученный AO.

Native endpoint должен быть в `D3D12_RESOURCE_STATE_COMMON` к началу копии; переходами своей DIRECT-очереди управляет техника. Для engine-текстур GPU 0 состояния и передачу между очередями согласует обёртка. Приёмнику нужны отслеживаемые ресурсы с `TEXCF_RTARGET`, `TEXCF_UNORDERED` или `TEXCF_UPDATE_DESTINATION`. Размер и формат согласуйте с каналом; отдельно проверяется `is_transfer_format_supported(...)`.

Передача в кадре использует GPU fences и не ждёт завершения GPU на CPU. Ticket `0` означает, что отправка не состоялась, в том числе из-за занятого слота; это не подтверждение готового результата. Обрабатывайте `busy` отдельно от настоящего отказа. AO при `busy` использует последнюю полученную карту, пока истории нет — считает текущий кадр на GPU 0. После resize и переключения режима/устройства история сбрасывается.

## 4. Минимальный пример из MG-12

В [`MultiGpuDemo::init/draw`](../engine/core/modules/graphics/src/render_window/multi_gpu_demo.cpp) Graphics использует готовый мост. Фрагмент инициализации ниже предполагает уже загруженные `const Shader* vertex`, `const Shader* pixel`; `m_producer` и `m_destination` — поля владельца. `m_destination` создаётся на GPU 0 как `TEXFMT_R8G8B8A8 | TEXCF_RTARGET`, размером 960×540.

```cpp
d3d::mgpu::FullscreenProducerDesc desc;
desc.width = 960;
desc.height = 540;
desc.vertexBytecode = vertex->bytecode.data();
desc.vertexBytecodeSize = vertex->bytecode.size();
desc.pixelBytecode = pixel->bytecode.data();
desc.pixelBytecodeSize = pixel->bytecode.size();
m_producer = d3d::mgpu::createFullscreenProducer(desc);
```

В кадре, при ненулевом `m_producer` и доступном GPU 1:

```cpp
const float constants[4] = {static_cast<float>(phase), 960.0f, 540.0f, 0.0f};
const d3d::mgpu::FullscreenResult result =
    d3d::mgpu::renderFullscreen(m_producer, constants, m_destination);
if (result == d3d::mgpu::FullscreenResult::submitted)
{
    d3d::stretch_rect(m_destination, backBuffer);
}
```

`renderFullscreen` уже объединяет draw на GPU 1, передачу MG-08/09 и приём на GPU 0. `submitted` означает поставленную в очереди работу; корректное чтение обеспечивает fence. Полная демка обрабатывает также `busy`/`failed`, отсутствие первого результата и рисование на GPU 0. При выходе вызывается `destroyFullscreenProducer(m_producer)` до остановки драйвера. Каждый кадр вызывается `collectFullscreenResources()`, в том числе при fallback; в `GraphicsImpl` это уже подключено и собирает также retired-ресурсы AO.

MG-12 переключается **F6**, AO — **F7** (сцена с AO / AO-карта / без AO) и **F8** (GPU 0 / GPU 1). Для AO отключите `sample.mgpuDemo.enabled`, иначе fullscreen-демка закроет сцену. Самостоятельный просмотр из корня репозитория:

```powershell
./tools/scripts/run_ssao_demo.ps1 -Device warp -Mode lit -Interactive -CaptureFrame 0
```

Для новой техники используйте аналогичный мост Render со своими входами и PSO; четырёх констант fullscreen-демки недостаточно для произвольного алгоритма. Контракт AO, включая матрицы текущего кадра, показан в [`SsaoConstants`](../engine/core/modules/render/include/nau/3d/multi_gpu_ssao_types.h). Перенесённый алгоритм разобран в [описании AO](ambient_occlusion.md); первоисточник — работа Богданова М.К.

## 5. Ограничения, замеры и завершение

| Область | Текущий контракт |
| --- | --- |
| Depth | Передаётся копией, не используется напрямую с другого устройства. AO отправляет raw reversed-Z `DEPTH32` в совместимый native `R32_FLOAT`; фон depth=0. Линеаризация выполняется в шейдере. Поддержана одна плоскость; combined depth-stencil для канала не подходит. |
| Форматы AO | Normals — `R16G16_FLOAT`, depth — 4 байта, результат — `R8_UNORM`: 9 логических байт/пиксель, около 18,66 МБ на 1080p без выравнивания и служебных копий. |
| Задержка | Свежий AO возвращается для resolve того же кадра, GPU 0 ждёт его на GPU. Фиксированной задержки в один кадр нет. При `busy` показывается старая карта; её возраст пока не ограничен, reprojection отсутствует. |
| Resize / shutdown | Сначала завершить работу каналов, затем освободить проход и ресурсы, затем аллокатор. Native pass умеет отложить освобождение незавершённой работы; уничтожение каналов может ждать на CPU. Не освобождайте память под работающим GPU. |
| Совместимость | В `mgpu/AO` расширены `receive_on_secondary` и `TransferChannelDesc`; пересоберите модули вместе. При переносе согласуйте изменения общих файлов с владельцами MG-04/08/13. |

Замеры включаются флагом **`-Profile`**; при прямом запуске exe — переменной `NAU_GPU_PROFILER=1` в запускающем процессе. Готовые команды для одной сцены:

```powershell
./tools/scripts/run_ssao_demo.ps1 -Device primary -Mode lit -Profile -CaptureFrame 0 -Seconds 45 -Label team
./tools/scripts/run_ssao_demo.ps1 -Device warp -Mode lit -Profile -CaptureFrame 0 -Seconds 45 -Label team
```

Для аппаратного GPU 1 замените `warp` на `secondary`. Запускайте сценарии последовательно: они временно меняют один `app.json` и восстанавливают его при выходе. CSV находятся в `build/ssao-runtime/*-profile-team-timings.csv`. Для сравнения режимов нужны отдельные запуски: колонка `mode` берётся из конфигурации и не меняется от F6/F8.

В AO MG-13 измеряет `ssao_raw`, `ssao_blur_h`, `ssao_blur_v` на GPU 1 и COPY-половины normals/depth/result. `/sum` — сумма GPU-времени двух копий; `/span` — калиброванный интервал между их началом и концом, включая промежуточное ожидание. COPY timestamps зависят от возможностей устройства. Новому каналу нужен observer в `TransferChannelDesc::profiler`; один вызов отправки сам по себе измерения не включает. Обычная fullscreen-демка MG-12 такого подключения пока не имеет.

Пример средних GPU-времён MG-13 для SceneBaseSample 634×611, `lit`, после исключения первых 10 кадров профайлера:

| Прогон | Frame GPU 0, мс | Возврат AO, `/sum`, мс |
| --- | ---: | ---: |
| RTX 4060 Ti, AO на GPU 0 | 1,1490 | — |
| RTX 4060 Ti + WARP, AO на WARP | 652,0625 | 93,5714 |

[Таблица замеров](ambient_occlusion_measurements.md) содержит числа для RTX 4060 Ti и WARP, среднее, медиану и P95; [описание профайлера](ambient_occlusion.md#профайлер-mg-13) — команды формирования сводки. Ускорение на аппаратной паре пока не подтверждено. В драйвере выбирается `VSYNCED`; сравнивать следует GPU-времена из CSV, учитывая, что `Frame` GPU 0 включает ожидание AO. Известны отдельное редкое падение при завершении асинхронной загрузки текстур и ошибка сохранения `cache/dx12.cache`; этот гайд не объявляет их исправленными.
