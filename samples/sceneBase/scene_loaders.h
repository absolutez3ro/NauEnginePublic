// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#pragma once
#include "nau/async/task.h"
#include "nau/scene/scene.h"

namespace nau::sample
{
    async::Task<> startupJungleRuins();

    /**
     */
    scene::IScene::Ptr makeTransformDemoScene(math::vec3 rootPos, bool addCamera);

    scene::IScene::Ptr makeInstancingDemoScene(math::vec3 rootPos, bool addCamera);
    scene::IScene::Ptr makeBillboardsScene(math::vec3 rootPos, bool addCamera);
}  // namespace nau::sample
