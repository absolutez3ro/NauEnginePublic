// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#pragma once
#include <EASTL/string.h>

#include "nau/app/application_delegate.h"

namespace nau::sample
{
    /**
     */
    class SampleAppDelegate : public ApplicationDelegate
    {
    public:
        /**
            @brief Initialize sample delegate
            @param sampleName Sample project's directory name (where project's CMakeLists.txt are located: [engine_root]/samples/[sampleName])
            @modulesList
        */
        SampleAppDelegate(eastl::string sampleName, eastl::string modulesList = getModulesList());

    protected:
        inline static eastl::string getModulesList()
        {
#if !defined(NAU_STATIC_RUNTIME) && defined(NAU_MODULES_LIST)
            return NAU_MODULES_LIST;
#else
            return {};
#endif
        }

        Result<> configureApplication() override;
        Result<> configureApplication(bool fileLoggingEnabled);

        eastl::string getModulesListString() const final;

        void onApplicationInitialized() override;

        const eastl::string m_sampleName;
        [[maybe_unused]] const eastl::string m_modulesList;
    };

}  // namespace nau::sample
