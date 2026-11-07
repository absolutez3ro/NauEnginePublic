// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#pragma once

#include "render_manager.h"
#include "render_view.h"
#include "billboards_manager.h"


#include "nau/math/math.h"
#include "nau/render/cascadeShadows.h"


namespace nau
{
    class BillboardsManager;

    using ViewKey = uint64_t;

    class RenderScene : public  IRefCounted
    {
        NAU_CLASS_(nau::RenderScene, IRefCounted);
    public:
        using Ptr = nau::Ptr<RenderScene>;

        async::Task<> initialize();

        eastl::shared_ptr<RenderList> collectRenderLists();
        eastl::vector<eastl::shared_ptr<RenderView>>& getViews();

        void addView(eastl::shared_ptr<RenderView> view);
        void addManager(nau::Ptr<IRenderManager> manager);

        template<class T>
        nau::Ptr<T> getManagerTyped()
        {
            for (auto manager : m_managers)
            {
                if (manager->is<T>())
                {
                    return manager;
                }
            }
            return nullptr;
        }

        nau::Ptr<BillboardsManager> getBillboardsManager();

        void updateViews(const nau::math::Matrix4& vp);
        void updateManagers();
        void renderScene(const nau::math::Matrix4& vp);
        void renderDepth(const nau::math::Matrix4& vp);
        void renderTranslucency(const nau::math::Matrix4& vp);
        void renderOutlineMask(const nau::math::Matrix4& vp);

        void renderBillboards(const nau::math::Matrix4& vp);

        MaterialAssetView::Ptr getZPrepassMaterial();

        struct Tags
        {
            static constexpr RenderTag opaqueTag = nau::strings::constHash("opaque");
            static constexpr RenderTag translucentTag = nau::strings::constHash("translucent");
            static constexpr RenderTag shadowCascadeTag = nau::strings::constHash("shadow_cascade");
            static constexpr RenderTag shadowOmniTag = nau::strings::constHash("shadow_omni");
        };

    private:
        eastl::vector<nau::Ptr<IRenderManager>> m_managers;
        eastl::vector<eastl::shared_ptr<RenderView>> m_views;

        nau::Ptr<BillboardsManager> m_billboardsManager;

        MaterialAssetView::Ptr m_zPrepassMaterial;
        MaterialAssetView::Ptr m_outlineMaterial;

        friend class RendferWindowImpl;
    };
}  // namespace nau
