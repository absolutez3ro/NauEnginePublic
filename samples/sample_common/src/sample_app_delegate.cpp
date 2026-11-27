// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#include "nau/samples/sample_app_delegate.h"

#include "nau/app/application_delegate.h"
#include "nau/app/global_properties.h"
#include "nau/input.h"

namespace nau::sample
{
    namespace
    {
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
        return configureApplication(true);
    }

    Result<> SampleAppDelegate::configureApplication(const bool fileLoggingEnabled)
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

        constexpr eastl::string_view logFilesProperty = "app/log/files";
        if (!fileLoggingEnabled && globalProperties.contains(logFilesProperty))
        {
            GlobalProperties::ModificationLock lock;
            const auto logFiles = globalProperties.getModify(logFilesProperty, lock);
            NauCheckResult(logFiles);
            (*logFiles)->as<RuntimeCollection&>().clear();
        }
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
        window.setVisible(true);
        const auto [width, height] = window.getClientSize();
        nau::input::setScreenResolution(width, height);
    }
}  // namespace nau::sample
