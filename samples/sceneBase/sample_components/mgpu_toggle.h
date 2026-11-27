// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#pragma once
#include "nau/scene/components/component.h"
#include "nau/scene/components/component_life_cycle.h"

namespace nau::sample
{
    class MgpuToggle final : public scene::Component,
                             public scene::IComponentUpdate
    {
    public:
        NAU_OBJECT(MgpuToggle, scene::Component, scene::IComponentUpdate)
        NAU_DECLARE_DYNAMIC_OBJECT

    private:
        void updateComponent(float dt) override;

        bool m_startApplied = false;
    };
}  // namespace nau::sample
