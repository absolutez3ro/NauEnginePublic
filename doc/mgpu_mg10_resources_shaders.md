# MG-10: ресурсы и шейдеры на втором GPU

Исследование и первый этап реализации для проверки 10.10.2026. Дата работы: **09.10.2026**.
Репозиторий: Nau Engine, ветка `mgpu/MG-10-resources-shaders-second_gpu`, исходный коммит `584eae21`.
Рабочее дерево перед началом было чистым. Работа выполнена в существующем клоне без переключения ветки и без push.

## Результат первой итерации

Добавлен настоящий C++-компонент `drv3d_dx12::DeviceResource`: он создаёт render-target текстуру и буферы
на явно переданном `ID3D12Device`, хранит владельца и освобождает COM-ресурсы. Проверены реальные операции
записи/чтения буферов и очистки/чтения текстуры на NVIDIA GeForce RTX 4060 Ti и WARP.

Это **подготовительный компонент для прохода GPU 1**, а не завершённый полноэкранный рендер.
В основной цикл движка его вызовы пока не добавлены. PSO, шейдер и производственная очередь GPU 1 ещё не реализованы.
На данном компьютере доступен один физический D3D12-адаптер: аппаратная приёмка GPU 1 не выполнена.

## Проверенная архитектура

Все пути ниже относительно корня репозитория; базовый каталог backend — `engine/core/modules/render/src/drv3d_DX12/`.

| Узел | Файл и реальные символы | Ограничение |
| --- | --- | --- |
| Глобальное состояние | `dx12.cpp`: `ApiState`, глобальный `api_state`, `drv3d_dx12::get_device()` | Один полный `Device`, один `FrontendState`, одна `ShaderProgramDatabase`. Внутренний accessor всегда возвращает `api_state.device`. |
| Дополнительное устройство | `multi_gpu_device.h`: `SecondaryGpuDevice`; `dx12.cpp`: `init_secondary_gpu()` | Уже есть отдельный native device, adapter, описание и capabilities. Полного `Device`, очередей, менеджера ресурсов и pipeline manager для него нет. |
| Выбор GPU | `dx12.cpp`: `check_and_add_adapter()`, `sort_adapters_by_perf()`, `sort_adapters_by_integrated()` | Учитываются поддержка D3D12, feature level и настройки драйвера. Кандидаты сортируются по памяти либо интегрированности. |
| Публичный доступ | `include/nau/3d/dag_drv3d.h`, `dx12.cpp`: `d3d::get_device(GpuId)`, `has_secondary_gpu()` | GPU 0 — основной device, GPU 1 — отдельный secondary device. Для недоступного/неизвестного GPU и вне инициализированного драйвера возвращается `nullptr`. Указатель заимствованный. |
| Текстуры | `texture.cpp`: `create_tex2d()`, `BaseTex`; `device.cpp`: `Device::createImage()` | В `texture.cpp` **46** вхождений `get_device(`, включая создание, upload/readback, копирование, ожидания и удаление. Замены одного места создания недостаточно. |
| Буферы | `dx12.cpp`: `d3d::create_vb()`, `create_ib()`, `create_cb()`, `create_sbuffer()`; `buffer.h`; `device.h`: `PlatformBufferInterfaceConfig::createBuffer()` | Фабрики и операции `BufferInterfaceConfigCommon` идут через основной device/context; `Device::createBuffer()` использует его аллокатор. Отдельного `buffer.cpp` здесь нет. |
| Память и descriptors | `device.h`, `resource_memory_heap.h/.cpp`, `resource_manager/image.h`, `resource_manager/basic_buffer.h` | `Device` владеет `ResourceMemoryHeap`, bindless-состоянием, очередями, context. `Image` связывает ресурс с памятью, views и global subresource IDs. |
| Шейдеры | `shader.h/.cpp`: `ShaderProgramDatabase::newRawVertexShader()`, `newRawPixelShader()`, `newGraphicsProgram()` | База хранит программы, IDs и layouts и передаёт модули через `DeviceContext`. В `ApiState` один экземпляр, вызовы в `dx12.cpp` используют основной context. |
| PSO | `device.h`: `PipelineManager pipeMan`, `PipelineCache pipelineCache`; `pipeline.cpp`: `PipelineManager::addGraphics()`, `createGraphics()` и `PipelineVariant::load()` | Native PSO создаются через переданный D3D12 device. Однако вызывающий `DeviceContext::ExecutionContext` передаёт именно `device.device`, его cache и framebuffer layouts. |
| Удаление | `device_context.cpp`: `destroyImage()`, `deleteTexture()`, `freeMemory()` | Освобождение привязано к завершению кадров основного context. Эти очереди удаления нельзя использовать для ресурсов независимого GPU 1. |
| Shutdown/recovery | `dx12.cpp`: `ApiState::releaseAll()`, обработка восстановления | Secondary device сбрасывается до teardown runtime и пересоздаётся после восстановления основного. Внешние потребители обязаны заранее освободить свои ресурсы. |

Счётчик можно воспроизвести:

```powershell
rg -c 'get_device\(' engine/core/modules/render/src/drv3d_DX12/texture.cpp
```

Путь основного рендера:

```text
d3d::create_tex / create_sbuffer
    -> api_state.device / drv3d_dx12::get_device()
    -> Device.resources + Device.context
    -> D3D12 resource, views, barriers, очередь удаления

d3d::create_*_shader / create_program
    -> api_state.shaderProgramDatabase
    -> DeviceContext -> Device.pipeMan + Device.pipelineCache
    -> root signature / native PSO на основном устройстве
```

`ShaderProgramDatabase` не является singleton по самому определению класса: единственен её экземпляр в `ApiState`.
CPU-байткод шейдера концептуально можно переиспользовать, но native PSO, root signatures, descriptors и команды
должны соответствовать устройству выполнения. Второй экземпляр базы без второго context/pipeline manager
не решит эту проблему.

GPU 1 не означает DXGI adapter index 1 или node mask 2. Существующий код исключает LUID основного адаптера
и software/WARP; выбирает первый успешно созданный подходящий device. Если primary задан явно и обычный список
пуст, выполняется отдельное перечисление. Недоступность GPU 1 оставляет основной рендер работающим.
Флаг `dx12.multiGpu` по умолчанию выключен. Описание предыдущего этапа: [multi_gpu_dx12.md](multi_gpu_dx12.md).
Приведённые там результаты от 02.10 получены на другой конфигурации и не являются результатами этой проверки.

## Сравнение решений

Количество файлов — предварительная оценка объёма законченного решения, а не результат реализации всех вариантов.

| Вариант | Сложность / ориентир файлов | Преимущества | Недостатки и риски | Соответствие архитектуре и сроку 21.10 |
| --- | --- | --- | --- | --- |
| А. Native D3D12 для GPU 1 | Низкая–средняя, около 4–8 | Использует готовый secondary device; ресурсы, PSO и небольшую очередь можно изолировать; основной рендер затрагивается мало. | Потребуются собственные descriptors, transitions, fence и cleanup. Нет автоматической интеграции с `BaseTex`, статистикой памяти, streaming и recovery. | Подходит для ограниченного демонстрационного прохода; наиболее реалистично к сроку. |
| Б. Явное устройство в engine resource API | Высокая, около 10–20+ | Правильная долговременная модель владения, естественный API для потребителей. | Нужны изменения создания, обновления, копирования, bindings, уничтожения и ожиданий. Один новый аргумент в `create_tex` создаст ложную поддержку. Риск регрессий GPU 0 высокий. | Архитектурно полезно, но полное внедрение до 21.10 рискованно. |
| В. Отдельные GPU-зависимые системы | Очень высокая, около 15–30+, пересекается с Б | Отдельные contexts, pools, descriptors, shader/pipeline managers дают основу полноценного multi-GPU. | Затрагивает глобальные IDs, frontend state, caches, scheduling, recovery. Второго `ShaderProgramDatabase` недостаточно. | Наиболее системный вариант, но для текущего минимального результата чрезмерен. |

### Выбранный подход

**А + ограниченный принцип Б:** маленький native путь, в котором каждый ресурс явно получает device и хранит
его как владельца. Следующий проход должен владеть собственной root signature, PSO, RTV heap, command allocator/list,
queue и fence. Глобальный `get_device()` не переключается; существующие engine textures/buffers не переиспользуются
на другом устройстве. Полное разделение систем из В откладывается до появления нескольких реальных потребителей.

Такой объём позволяет получить целевой полноэкранный проход без миграции всего `BaseTex` и системы шейдерных IDs.
Это инженерная оценка, а не гарантия срока: нужен доступ к компьютеру с двумя физическими D3D12 GPU для приёмки.
Cross-adapter sharing и показ результата на GPU 0 не требуются, чтобы доказать запись шейдера в собственную текстуру GPU 1.

## Реализованный компонент и контракт

Новые [device_resource.h](../engine/core/modules/render/src/drv3d_DX12/device_resource.h) и
[device_resource.cpp](../engine/core/modules/render/src/drv3d_DX12/device_resource.cpp) содержат `DeviceResource`:

- `createRenderTarget(device, width, height)` создаёт committed 2D-текстуру `R8G8B8A8_UNORM`, один mip,
  один sample, `ALLOW_RENDER_TARGET`, default heap, начальное состояние `COMMON`.
- `createBuffer(device, size, heapType)` создаёт committed buffer в `DEFAULT`, `UPLOAD` или `READBACK` heap.
  Начальные состояния соответственно `COMMON`, `GENERIC_READ`, `COPY_DEST`.
- COM-ссылки на ресурс и его конкретный device принадлежат объекту. Копирование запрещено. При уничтожении
  и `reset()` ресурс освобождается раньше ссылки на device; повторный reset допустим.
- Нулевое устройство, нулевые/слишком большие размеры текстуры, нулевой буфер и неподдерживаемый heap type
  отвергаются. Ошибка D3D12 возвращается вызывающему коду. Fallback на GPU 0 отсутствует.
- Повторное создание в занятом объекте отвергается с `DXGI_ERROR_INVALID_CALL`: рабочий ресурс нельзя незаметно
  заменить и освободить. Ошибка не меняет существующего владельца/ресурс. Повторное использование требует явного reset.
- `getDevice()` и `getResource()` возвращают заимствованные указатели. `getInitialState()` описывает только
  состояние при создании: последующие barriers обязан учитывать владелец прохода.
- Node masks равны 1 для node 0 **выбранного device**. Shared/cross-adapter flags не включаются.

Компонент находится внутри DX12 backend. Его `.cpp` подхватывается существующим `nau_collect_files()` в
`render/src/CMakeLists.txt` при повторной конфигурации. В standalone-тестах компилируется этот же production `.cpp`.
Нового публичного `d3d::create_tex(..., GpuId)` на данном этапе нет.

Передача результата существующего `d3d::get_device(d3d::SECONDARY_GPU)` в методы компонента — точка подключения
будущего внутреннего прохода. Вызывать это нужно в рамках жизненного цикла драйвера из потока рендера.
Если accessor возвращает `nullptr`, проход должен отказаться от запуска. Сам компонент не проверяет глобальный
статус драйвера, что также позволяет тестировать его на независимом native устройстве.

**Время жизни GPU-работы:** COM-владение не заменяет fence. В компоненте нет скрытых submit/wait и нет очереди
отложенного удаления. Владелец обязан завершить все обращения GPU перед reset/destruction, затем освободить
ресурсы до driver shutdown/recovery. Создание/использование/reset одного объекта синхронизируются вызывающим кодом.
В тестах ожидание fence выполняется до чтения и освобождения. Производственный lifecycle прохода — следующая задача.

Использованы правила Microsoft для [CreateCommittedResource](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createcommittedresource),
[описаний ресурсов](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_resource_desc),
[node masks](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_heap_properties)
и [ожидания fence](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-seteventoncompletion).

## Затронутые файлы

| Файл | Изменение |
| --- | --- |
| `engine/core/modules/render/src/drv3d_DX12/device_resource.h` | Новый контракт явного владения native-ресурсом. |
| `engine/core/modules/render/src/drv3d_DX12/device_resource.cpp` | Создание текстуры/буферов, проверки входа, управление COM. |
| `engine/core/modules/render/tests/test_multi_gpu/test_device_resource.cpp` | Новые проверки на WARP и аппаратном устройстве, реальные transfers и readback, диагностика ресурсов. |
| `engine/core/modules/render/tests/test_multi_gpu/test_multi_gpu.cpp` | Существующий тест пары физических GPU теперь также создаёт текстуру и буфер GPU 1 и проверяет `ID3D12Resource::GetDevice()`. |
| `engine/core/modules/render/tests/test_multi_gpu/CMakeLists.txt` | Production `.cpp` и новые тесты в target; читаемые имена WARP/Hardware; timeout 30 секунд для каждого CTest. |
| `doc/mgpu_mg10_resources_shaders.md` | Исследование, решение, контракт и результаты. |

Исходные `dx12.cpp`, `texture.cpp`, `device.h/.cpp`, `shader.h/.cpp`, `pipeline.cpp`, настройки приложения
и выбор GPU 0 не изменены. Для следующего шага предполагается добавить отдельный компонент прохода и его HLSL,
затем точечно подключить opt-in запуск и lifecycle в `dx12.cpp`/`SecondaryGpuDevice` и проверочный sample.
`ShaderProgramDatabase` и основной `PipelineManager` для одного native PSO менять не потребуется.

## Проверки 09.10.2026

Среда: Windows, MSVC **19.51.36260**, toolset **14.51.36231**, Windows SDK **10.0.26100.0**.
Аппаратный адаптер: **NVIDIA GeForce RTX 4060 Ti**, драйвер **32.0.16.1714**. WARP используется отдельно как программное устройство.

| Проверка | Фактический результат |
| --- | --- |
| Базовые тесты до правок | 6 passed, 2 skipped: отсутствует пара физических адаптеров. |
| Сборка `test_multi_gpu`, Debug x64 после правок | Успешна; компилируется production `device_resource.cpp`. |
| CTest после правок | **19 passed, 2 skipped, 0 failed** из 21. Все 13 новых тестовых случаев прошли. |
| Buffer round-trip | Проверены 1024 байта `UPLOAD -> DEFAULT -> READBACK`, на WARP и NVIDIA. |
| Render target | Очистка в magenta, копирование в readback, проверка всех 37×19 пикселей с учётом row pitch; WARP и NVIDIA. Это clear, не draw/шейдер. |
| Владение | Проверены native `GetDevice()`, сохранение ресурса при ошибке повторной инициализации, reset/reuse и жизнь device после снятия внешней ссылки. |
| Изоляция устройств | Одновременно созданы независимые WARP и hardware devices, ресурсы сохраняют правильного владельца. Это не подмена двух физических GPU. |
| D3D12 debug layer | Доступен. Новые параметризованные тесты проверяют отсутствие ERROR/CORRUPTION и оставшихся resources/heaps через `ReportLiveDeviceObjects`. |
| Два физических GPU | Оба существующих hardware pair теста пропущены; ветвь создания ресурсов физического GPU 1 добавлена, но здесь не исполнена. |
| Полная конфигурация движка | Остановилась в `cmake/NauGenFunctions.cmake`: обязательный Python-пакет `cymbal` отсутствует. Полный Render/SceneBaseSample не собран и не запущен. |
| `git diff --check` | Без ошибок пробелов. |

Сохранение GPU 0 проверено на уровне неизменности существующего production-пути и старых компонентных тестов.
Визуальная регрессионная проверка основной сцены **не выполнена**; успешный component test не доказывает её прохождение.
Принудительные OOM/device removal, recovery под нагрузкой и гонки потоков не проверялись.

Воспроизведение из корня, в консоли с CMake в PATH:

```powershell
cmake -S engine/core/modules/render/tests/test_multi_gpu -B build/multi_gpu_tests -G "Visual Studio 18 2026" -A x64
cmake --build build/multi_gpu_tests --config Debug --target test_multi_gpu --parallel 4
ctest --test-dir build/multi_gpu_tests -C Debug --output-on-failure
```

Для VS 2022 следует выбрать генератор `Visual Studio 17 2022` и отдельный build-каталог.
Локальный журнал: `build/multi_gpu_tests/Testing/Temporary/LastTest.log` (build-файлы не включены в коммит).
Timeout CTest ограничивает зависание драйвера при тестовом ожидании fence; запуск executable напрямую этого ограничения не имеет.

### Приведение стиля к руководству — 09.10.2026

Новые `device_resource.h/.cpp`, `test_device_resource.cpp` и добавленная проверка ресурсов в
`test_multi_gpu.cpp` приведены к `doc/coding_style_guide.md`: camelCase, поля с `m_`, явные специальные
методы класса, Doxygen, явные типы вместо избыточного `auto`, Allman и отступы по четыре пробела.
Getters `DeviceResource` перенесены в `.cpp`. В тестах используются `eastl::array` и
`eastl::unique_ptr<char[]>` для массива диагностического сообщения; они не требуют сборки NauKernel.
Проверка физического GPU 1 вынесена в небольшой helper; соседний код предыдущих MG-задач не переформатирован.

Специализированные COM-указатели `Microsoft::WRL::ComPtr` сохранены ради `AddRef/Release` и совместимости
с существующим `SecondaryGpuDevice`. Native `HRESULT` сохранён для ошибок D3D12, исключения не добавлены.
Имена переопределений GoogleTest `SetUp`/`TearDown` и существующих внешних API следуют их контрактам.
Это обоснованные особенности используемых API, а не новые общие соглашения об именовании или владении.

В комплектном EASTL обнаружен необъявленный `pName` в debug-ветке `allocator::realloc`.
Для `test_multi_gpu` задан `EASTL_DEBUGPARAMS_LEVEL=0`: неиспользуемые параметры имён аллокаций отключены,
assertions EASTL сохранены, сторонние исходники не изменены.

После правок повторно прошли Debug-сборка и CTest: **19 passed, 2 skipped, 0 failed**.
Для трёх новых C++-файлов прошёл `clang-format 22.1.3 --style=file --dry-run --Werror` с конфигурацией
репозитория; `git diff --check` также прошёл. Ограничения полной сборки и проверки второго физического GPU
остаются указанными выше.

Команда попытки полной конфигурации:

```powershell
cmake -S . -B build/mg10_engine_check -G "Visual Studio 18 2026" -A x64 -DBUILD_SHARED_LIBS=OFF -DNAU_CORE_TOOLS=OFF -DNAU_CORE_SAMPLES=OFF -DNAU_CORE_TESTS=OFF
```

## Оставшаяся работа и план до 21.10

| Даты | Результат этапа |
| --- | --- |
| 09–10.10 | Представить этот отчёт, выбранный подход и проверенный компонент. Организовать доступ к двум физическим D3D12 GPU; запустить расширенные hardware pair тесты. |
| 11–13.10 | Добавить owner прохода GPU 1: direct queue, allocator/list, fence, RTV heap; явные barriers и штатное завершение перед освобождением. Восстановить доступность полной сборки. |
| 14–16.10 | Добавить минимальный VS fullscreen triangle + PS, отдельные root signature и graphics PSO на GPU 1; цвет/параметр передавать через созданный upload/constant buffer. Настроить компиляцию HLSL штатным DXC. |
| 17–18.10 | Выполнить draw в собственную `DeviceResource`-текстуру GPU 1, дождаться fence и проверить readback по ожидаемым пикселям. Подключить opt-in запуск в движке с обработкой отсутствия GPU 1. |
| 19–20.10 | Проверить основную сцену с флагом off/on, закрытие, повторный запуск и lifecycle recovery; debug layer на обоих физических GPU, проверить принадлежность PSO и ресурсов. |
| 21.10 | Финальная демонстрация: имена/LUID двух физических GPU, успешное создание texture/buffer/PSO на GPU 1, результат именно draw в его текстуре, чистое завершение. |

Критерий готовности MG-10: на **втором физическом устройстве** созданы texture, buffer и PSO;
полноэкранный VS/PS записывает ожидаемое изображение в его собственную текстуру; результат проверен readback;
всё освобождается после завершения команд, основной рендер продолжает работать.
Текущая итерация закрывает исследование и подготовку ресурсов, но этот финальный критерий пока не достигнут.
