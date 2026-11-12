// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#include "render_scene.h"

#include <chrono>

#include "nau/utils/performance_profiling.h"

#include "nau/render/cascadeShadows.h"

namespace nau
{
    async::Task<> RenderScene::initialize()
    {
        MaterialAssetRef outlineMaterialAssetRef = AssetPath{"file:/res/materials/outline_calculation.nmat_json"};
        auto outlineMaterialTask = outlineMaterialAssetRef.getAssetViewTyped<MaterialAssetView>();

        MaterialAssetRef assetRef = AssetPath{"file:/res/materials/z_prepass.nmat_json"};
        auto zPrepassMaterialTask = assetRef.getAssetViewTyped<MaterialAssetView>();

        nau::MaterialAssetRef billboardsMatRef = AssetPath{"file:/res/materials/billboards.nmat_json" };
        auto billboardsMaterialTask = billboardsMatRef.getAssetViewTyped<MaterialAssetView>();

        co_await async::whenAll(Expiration::never(), zPrepassMaterialTask, billboardsMaterialTask, outlineMaterialTask);

        m_zPrepassMaterial = *zPrepassMaterialTask;
        m_outlineMaterial = *outlineMaterialTask;

        NAU_ASSERT(*billboardsMaterialTask);
        m_billboardsManager = rtti::createInstance<BillboardsManager>(*billboardsMaterialTask);

        co_return;
    }

    eastl::shared_ptr<RenderList> RenderScene::collectRenderLists()
    {
        return eastl::shared_ptr<RenderList>();
    }

    eastl::vector<eastl::shared_ptr<RenderView>>& RenderScene::getViews()
    {
        return m_views;
    }

    void RenderScene::addView(eastl::shared_ptr<RenderView> view)
    {
        NAU_ASSERT(view);
        m_views.push_back(view);
    }

    void RenderScene::addManager(nau::Ptr<IRenderManager> manager)
    {
        NAU_ASSERT(manager);
        m_managers.push_back(manager);
    }

    nau::Ptr<BillboardsManager> RenderScene::getBillboardsManager()
    {
        return m_billboardsManager;
    }

    void RenderScene::updateView(RenderView& view, const nau::math::Vector3& viewerPosition)
    {
        const auto start = std::chrono::steady_clock::now();

        view.clearLists();
        for (auto& manager : m_managers)
        {
            view.addRenderList(manager->getRenderList(viewerPosition, view.getInstanceFilter(), view.getMaterialFilter(),
                                                      view.getLodSelection()));
        }
        view.prepareInstanceData();

        uint64_t instances = 0;
        uint64_t triangles = 0;
        for (const auto& list : view.getLists())
        {
            for (const auto& entity : list->getEntities())
            {
                instances += entity.instancesCount;
                if (entity.endIndex > entity.startIndex)
                {
                    triangles += uint64_t(entity.endIndex - entity.startIndex) / 3 * entity.instancesCount;
                }
            }
        }
        const ViewsStats::Kind kind = view.containsTag(Tags::shadowCascadeTag) ? ViewsStats::Cascade
                                      : view.containsTag(Tags::shadowOmniTag)  ? ViewsStats::Omni
                                                                                : ViewsStats::Main;
        m_viewsStats.seconds[kind] += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        m_viewsStats.instances[kind] += instances;
        if (kind == ViewsStats::Cascade)
        {
            const intptr_t cascade = reinterpret_cast<intptr_t>(view.getUserData());
            if (cascade >= 0 && cascade < static_cast<intptr_t>(csm::CascadeShadows::MAX_CASCADES))
            {
                m_viewsStats.cascadeInstances[cascade] += instances;
                m_viewsStats.cascadeTriangles[cascade] += triangles;
            }
        }
    }

    void RenderScene::updateViews(const nau::math::Matrix4& vp, const nau::math::Vector3& viewerPosition)
    {
        m_viewsStats = {};
        for (auto& view : m_views)
        {
            if (!view->isActive())
            {
                // Its lists of an earlier frame must not be drawn.
                view->clearLists();
                continue;
            }
            updateView(*view, viewerPosition);
        }
    }

    void RenderScene::updateManagers()
    {
        for (auto& manager : m_managers)
        {
            manager->update();
        }

        m_billboardsManager->update();
    }

    void RenderScene::renderScene(const nau::math::Matrix4& vp)
    {
        for (auto& view : m_views)
        {
            if (view->containsTag(Tags::opaqueTag))
            {
                view->renderInstanced(vp);
            }
        }
    }

    void RenderScene::renderDepth(const nau::math::Matrix4& vp)
    {
        for (auto& view : m_views)
        {
            if (view->containsTag(Tags::opaqueTag))
            {
                view->renderZPrepass(vp, m_zPrepassMaterial.get());
            }
        }
    }

    void RenderScene::renderTranslucency(const nau::math::Matrix4& vp)
    {
        for (auto& view : m_views)
        {
            if (view->containsTag(Tags::translucentTag))
            {
                view->renderInstanced(vp);
            }
        }
    }

    void RenderScene::renderOutlineMask(const nau::math::Matrix4& vp)
    {
        for (auto& view : m_views)
        {
            view->renderZPrepass(vp, m_outlineMaterial.get());
        }
    }

    void RenderScene::renderBillboards(const nau::math::Matrix4& vp)
    {
        NAU_ASSERT(m_billboardsManager);

        m_billboardsManager->render(vp);
    }

    MaterialAssetView::Ptr RenderScene::getZPrepassMaterial()
    {
        return m_zPrepassMaterial;
    }

}  // namespace nau
