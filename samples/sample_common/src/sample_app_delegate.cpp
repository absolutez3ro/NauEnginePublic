// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#include "nau/samples/sample_app_delegate.h"

#include "nau/app/application_delegate.h"
#include "nau/app/application.h"
#include "nau/app/global_properties.h"
#include "nau/input.h"

#ifdef _WIN32
#include "nau/platform/windows/app/windows_window.h"
#endif

namespace nau::sample
{
    namespace
    {
#ifdef _WIN32
        bool configureAutomationWindow(IPlatformWindow& window, GlobalProperties& properties)
        {
            const unsigned width = properties.getValue<unsigned>("sample/automation/width").value_or(1280u);
            const unsigned height = properties.getValue<unsigned>("sample/automation/height").value_or(720u);
            const auto requestedMode = properties.getValue<eastl::string>("sample/automation/windowMode").value_or("auto");
            auto* nativeWindow = window.as<IWindowsWindow*>();
            MONITORINFO monitor{sizeof(MONITORINFO)};
            const HWND hwnd = nativeWindow ? nativeWindow->getWindowHandle() : nullptr;
            if (!hwnd || !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
            {
                NAU_LOG_ERROR("AUTORUN_WINDOW_ERROR: Cannot query the window monitor");
                return false;
            }
            if (GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(hwnd)) == DPI_AWARENESS_UNAWARE)
            {
                NAU_LOG_ERROR("AUTORUN_WINDOW_ERROR: Windows forces DPI-unaware scaling; disable the compatibility DPI override for this benchmark");
                return false;
            }
            const auto monitorWidth = static_cast<unsigned>(monitor.rcMonitor.right - monitor.rcMonitor.left);
            const auto monitorHeight = static_cast<unsigned>(monitor.rcMonitor.bottom - monitor.rcMonitor.top);
            if (width < 320 || height < 240 || width > monitorWidth || height > monitorHeight)
            {
                NAU_LOG_ERROR("AUTORUN_WINDOW_ERROR: Requested {}x{}, monitor {}x{}; the requested client area does not fit",
                    width, height, monitorWidth, monitorHeight);
                return false;
            }
            if (requestedMode != "auto" && requestedMode != "borderless" && requestedMode != "windowed")
            {
                NAU_LOG_ERROR("AUTORUN_WINDOW_ERROR: Unknown windowMode {}; use auto, borderless or windowed", requestedMode.c_str());
                return false;
            }
            const DWORD originalStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
            const DWORD originalExStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
            const DWORD framedStyle = (originalStyle & ~WS_POPUP) | WS_OVERLAPPEDWINDOW;
            const UINT dpi = GetDpiForWindow(hwnd);
            RECT frame{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
            if (!AdjustWindowRectExForDpi(&frame, framedStyle, GetMenu(hwnd) != nullptr, originalExStyle, dpi))
            {
                NAU_LOG_ERROR("AUTORUN_WINDOW_ERROR: Cannot calculate the client area at DPI {}", dpi);
                return false;
            }
            const bool frameFits = frame.right - frame.left <= monitor.rcWork.right - monitor.rcWork.left &&
                frame.bottom - frame.top <= monitor.rcWork.bottom - monitor.rcWork.top;
            const bool borderless = requestedMode == "borderless" || (requestedMode == "auto" && !frameFits);
            if (!borderless && !frameFits)
            {
                NAU_LOG_ERROR("AUTORUN_WINDOW_ERROR: Framed {}x{} does not fit the monitor work area; use windowMode=auto or borderless", width, height);
                return false;
            }
            const DWORD style = borderless ? (originalStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP : framedStyle;
            const DWORD exStyle = borderless ? originalExStyle & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME | WS_EX_STATICEDGE) : originalExStyle;
            SetLastError(ERROR_SUCCESS);
            const auto previousStyle = SetWindowLongPtrW(hwnd, GWL_STYLE, style);
            const bool styleFailed = previousStyle == 0 && GetLastError() != ERROR_SUCCESS;
            SetLastError(ERROR_SUCCESS);
            const auto previousExStyle = SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
            if (styleFailed || (previousExStyle == 0 && GetLastError() != ERROR_SUCCESS))
            {
                NAU_LOG_ERROR("AUTORUN_WINDOW_ERROR: Cannot set the requested window style");
                return false;
            }
            const RECT area = borderless ? monitor.rcMonitor : monitor.rcWork;
            const int outerWidth = borderless ? static_cast<int>(width) : frame.right - frame.left;
            const int outerHeight = borderless ? static_cast<int>(height) : frame.bottom - frame.top;
            if (!SetWindowPos(hwnd, nullptr, area.left + (area.right - area.left - outerWidth) / 2,
                    area.top + (area.bottom - area.top - outerHeight) / 2, outerWidth, outerHeight,
                    SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE))
            {
                NAU_LOG_ERROR("AUTORUN_WINDOW_ERROR: Cannot resize the benchmark window");
                return false;
            }
            const auto [clientWidth, clientHeight] = window.getClientSize();
            if (clientWidth != width || clientHeight != height)
            {
                NAU_LOG_ERROR("AUTORUN_WINDOW_ERROR: Requested {}x{}, received client {}x{}", width, height, clientWidth, clientHeight);
                return false;
            }
            properties.setValue("sample/automation/actualWindowMode", eastl::string{borderless ? "borderless" : "windowed"}).ignore();
            NAU_LOG_INFO("AUTORUN_WINDOW: mode={}, requested={}x{}, client={}x{}, monitor={}x{}, dpi={}",
                borderless ? "borderless" : "windowed", width, height, clientWidth, clientHeight, monitorWidth, monitorHeight, dpi);
            return true;
        }
#endif

        std::filesystem::path findSampleDirPath(const eastl::string_view sampleName)
        {
            namespace fs = std::filesystem;
            // lookup project's root directory: require presence all specific directories
            // Need to distinguish the project root directory from the cmake build directory (where some directories with the same name are also present)
            const fs::path projectRelativeDir = ::fmt::format("samples/{}", sampleName);
            const std::array requiredSubPaths = {
                fs::path{"CMakeLists.txt"},
            };

            fs::path currentPath = fs::current_path();

            do
            {
                fs::path targetPath = currentPath / projectRelativeDir;
                if (fs::exists(targetPath))
                {
                    const bool isTargetPathOk = std::all_of(requiredSubPaths.begin(), requiredSubPaths.end(), [&targetPath](const fs::path& subPath)
                    {
                        const fs::path lookupPath = targetPath / subPath;
                        return fs::exists(lookupPath);
                    });

                    if (isTargetPathOk)
                    {
                        return fs::canonical(targetPath);
                    }
                }

                currentPath = currentPath.parent_path();

            } while (currentPath.has_relative_path());

            return {};
        }
    }  // namespace

    SampleAppDelegate::SampleAppDelegate(eastl::string sampleName, eastl::string modulesList) :
        m_sampleName(std::move(sampleName)),
        m_modulesList(std::move(modulesList))
    {
    }

    Result<> SampleAppDelegate::configureApplication()
    {
        namespace fs = std::filesystem;

        getServiceProvider().get<GlobalProperties>().addVariableResolver("sampleDir", [](eastl::string_view sampleName) -> eastl::optional<eastl::string>
        {
            if (const auto samplePath = findSampleDirPath(sampleName); !samplePath.empty())
            {
                const std::string pathStr = samplePath.string();
                return eastl::string{pathStr.data(), pathStr.size()};
            }

            return eastl::nullopt;
        });

        const auto projectRootDir = findSampleDirPath(m_sampleName);
        if (projectRootDir.empty())
        {
            return NauMakeError("Fail to locate project root dir.");
        }

        auto wcsProjectDir = projectRootDir.wstring();
        auto utf8ProjectDir = strings::wstringToUtf8(eastl::wstring_view{wcsProjectDir.data(), wcsProjectDir.size()});

        GlobalProperties& globalProperties = getServiceProvider().get<GlobalProperties>();

        globalProperties.setValue("sampleProjectDir", std::move(utf8ProjectDir)).ignore();

        for (auto entry : fs::directory_iterator{projectRootDir / "config"})
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            if (strings::icaseEqual(entry.path().extension().wstring(), L".json"))
            {
                NauCheckResult(mergePropertiesFromFile(globalProperties, entry.path()));
            }
        }

#ifdef _WIN32
        // Set awareness before the sample creates its native window. This affects this process only.
        if (globalProperties.getValue<bool>("sample/automation/enabled").value_or(false) &&
            !SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) && GetLastError() != ERROR_ACCESS_DENIED)
        {
            NAU_LOG_WARNING("AUTORUN_WINDOW: Cannot enable per-monitor DPI awareness, error={}", GetLastError());
        }
#endif
        return nau::applyDefaultAppConfiguration();
    }

    eastl::string SampleAppDelegate::getModulesListString() const
    {
#if !defined(NAU_STATIC_RUNTIME)
        return m_modulesList;
#else
        return {};
#endif
    }

    /**
     */
    void SampleAppDelegate::onApplicationInitialized()
    {
        auto& windowService = getServiceProvider().get<IWindowManager>();
        auto& window = windowService.getActiveWindow();
        auto& properties = getServiceProvider().get<GlobalProperties>();
        if (properties.getValue<bool>("sample/automation/enabled").value_or(false))
        {
#ifdef _WIN32
            if (!configureAutomationWindow(window, properties))
            {
                getApplication().stop();
                return;
            }
#else
            const unsigned width = eastl::clamp(properties.getValue<unsigned>("sample/automation/width").value_or(1280u), 320u, 3840u);
            const unsigned height = eastl::clamp(properties.getValue<unsigned>("sample/automation/height").value_or(720u), 240u, 2160u);
            // setSize takes outer dimensions; compensate once to obtain the requested client area.
            window.setSize(width, height);
            const auto [clientWidth, clientHeight] = window.getClientSize();
            window.setSize(width + (width-clientWidth), height + (height-clientHeight));
#endif
        }
        window.setVisible(true);
        const auto [width, height] = window.getClientSize();
        nau::input::setScreenResolution(width, height);
    }
}  // namespace nau::sample
