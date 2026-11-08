// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#include "./asset_descriptor_impl.h"

#include "./asset_manager_impl.h"
#include "nau/assets/asset_messages.h"

namespace nau
{
    /**
     */
    class InnerAssetDescriptor : public IAssetDescriptor,
                                 public assets::IAssetDescriptorInternal
    {
        NAU_CLASS_(nau::InnerAssetDescriptor, IAssetDescriptor, assets::IAssetDescriptorInternal)

    public:
        InnerAssetDescriptor(AssetDescriptorImpl* parentAsset, eastl::string_view innerPath) :
            m_parentAsset(parentAsset),
            m_assetInnerPath(innerPath),
            m_assetId(AssetManagerImpl::getInstance().getNextAssetId()),
            m_assetFullPath{parentAsset->getAssetPath().setAssetInnerPath(innerPath)}

        {
            NAU_FATAL(m_parentAsset);
            NAU_ASSERT(!m_assetInnerPath.empty());
        }

        ~InnerAssetDescriptor()
        {
        }

    private:
        uint64_t getAssetId() const override
        {
            return m_assetId;
        }

        AssetPath getAssetPath() const override
        {
            return m_assetFullPath;
        }

        async::Task<IAssetView::Ptr> getAssetView(const AssetViewDescription& viewDescription) override
        {
            NAU_FATAL(m_parentAsset);
            return m_parentAsset->getInnerAssetView(m_assetInnerPath, viewDescription.viewApi);
        }

        async::Task<ReloadableAssetView::Ptr> getReloadableAssetView(const AssetViewDescription& viewDescription) override
        {
            NAU_FATAL(m_parentAsset);
            return m_parentAsset->getInnerReloadableAssetView(m_assetInnerPath, viewDescription.viewApi);
        }

        async::Task<nau::Ptr<>> getRawAsset() override
        {
            NAU_FATAL(m_parentAsset);
            return m_parentAsset->getInnerRawAsset(m_assetInnerPath);
        }

        eastl::optional<assets::AssetInternalState> getCachedAssetViewInternalState(const rtti::TypeInfo* viewType, assets::InternalStateOptsFlag opts) override
        {
            NAU_FATAL(m_parentAsset);
            return m_parentAsset->getInnerCachedAssetViewState(m_assetInnerPath, viewType, opts);
        }

        void load() override
        {
            NAU_FATAL(m_parentAsset);
            m_parentAsset->load();
        }

        UnloadResult unload() override
        {
            NAU_FATAL(m_parentAsset);
            return m_parentAsset->unload();
        }

        LoadState getLoadState() const override
        {
            NAU_FATAL(m_parentAsset);
            return m_parentAsset->getLoadState();
        }

        const nau::Ptr<AssetDescriptorImpl> m_parentAsset;
        const eastl::string m_assetInnerPath;
        const AssetId m_assetId;
        const AssetPath m_assetFullPath;
    };

    AssetDescriptorImpl::AssetViewEntry::AssetViewEntry(eastl::string assetInnerPath, const rtti::TypeInfo* viewType) :
        m_assetInnerPath(std::move(assetInnerPath)),
        m_viewType(viewType)
    {
    }

    AssetDescriptorImpl::AssetViewEntry::~AssetViewEntry()
    {
#ifdef NAU_ASSERT_ENABLED
        lock_(m_mutex);
        NAU_FATAL(!m_assetViewCreationState || m_assetViewCreationState.isReady(), "Destroying asset cache, but it is still loading/fabricating");
#endif
    }

    bool AssetDescriptorImpl::AssetViewEntry::isThatView(eastl::string_view assetInnerPath, const rtti::TypeInfo* viewType) const
    {
        if (m_assetInnerPath != assetInnerPath)
        {
            return false;
        }

        if (m_viewType != nullptr && viewType != nullptr)
        {
            return *m_viewType == *viewType;
        }

        return m_viewType == nullptr && viewType == nullptr;
    }

    bool AssetDescriptorImpl::AssetViewEntry::hasNoAssetViewReferences()
    {
        lock_(m_mutex);
        return m_assetViewRef.isDead();
    }

    async::Task<IAssetView::Ptr> AssetDescriptorImpl::AssetViewEntry::fabricateAssetView(IAssetContainer& container)
    {
        IAssetView::Ptr assetView;

        nau::Ptr<> asset = container.getAsset(m_assetInnerPath);
        NAU_ASSERT(asset);
        if (!asset)
        {
            co_return nullptr;
        }

        if (!m_viewType)
        {
            NAU_FATAL(asset->is<IAssetView>(), "Default view type currently not implemented, view type must be explicitly specified !");
            assetView = std::move(asset);
        }
        else if (asset->is(*m_viewType))
        {
            NAU_ASSERT(asset->is<IAssetView>());
            assetView = std::move(asset);
        }
        else
        {
            IAssetViewFactory* const viewFactory = AssetManagerImpl::getInstance().findAssetViewFactory(*m_viewType);
            NAU_ASSERT(viewFactory, "Don't known how to create requested asset view: ({})", m_viewType->getTypeName());
            if (viewFactory)
            {
                assetView = co_await viewFactory->createAssetView(asset, *m_viewType);
            }
        }

        if (!assetView)
        {
            NAU_LOG_WARNING(u8"Fail to fabricate asset view ({})", m_assetInnerPath);
        }

        co_return assetView;
    }

    async::Task<IAssetView::Ptr> AssetDescriptorImpl::AssetViewEntry::getAssetView(IAssetContainer& container)
    {
        bool needToFabricateAssetView = false;
        {
            lock_(m_mutex);
            if (auto assetView = m_assetViewRef.lock(); assetView)
            {
                co_return assetView;
            }

            if (needToFabricateAssetView = !m_assetViewCreationState; needToFabricateAssetView)
            {  // This is the first async request to the specified asset view.
                m_assetViewCreationState.emplace();
                m_assetViewCreationState.setAutoResetOnReady(true);
            }
        }

        NAU_FATAL(m_assetViewCreationState);

        IAssetView::Ptr assetView;

        if (needToFabricateAssetView)
        {
            assetView = co_await fabricateAssetView(container);
            {
                // WeakPtr is not atomic: other requests read it under this lock.
                lock_(m_mutex);
                m_assetViewRef = assetView;
            }
            m_assetViewCreationState.resolve(assetView);
        }
        else
        {
            // this is subsequent request for the asset view (which is already fabricating): just need to wait for the result of operation.
            assetView = co_await m_assetViewCreationState.getNextTask();
        }

        co_return assetView;
    }

    async::Task<ReloadableAssetView::Ptr> AssetDescriptorImpl::AssetViewEntry::getReloadableAssetView(IAssetContainer& container)
    {
        {
            lock_(m_mutex);
            if (auto reloadableAssetView = m_reloadableAssetViewRef.lock(); reloadableAssetView)
            {
                co_return reloadableAssetView;
            }
        }

        auto assetView = co_await getAssetView(container);

        ReloadableAssetView::Ptr reloadableAssetViewPtr;
        {
            // Several requests can wait for the same view (two materials with one texture): they share one wrapper.
            lock_(m_mutex);
            reloadableAssetViewPtr = m_reloadableAssetViewRef.lock();
            if (!reloadableAssetViewPtr)
            {
                reloadableAssetViewPtr = rtti::createInstance<ReloadableAssetView>();
                reloadableAssetViewPtr->reloadAssetView(assetView);
                m_reloadableAssetViewRef = reloadableAssetViewPtr;
            }
        }

        co_return reloadableAssetViewPtr;
    }

    async::Task<> AssetDescriptorImpl::AssetViewEntry::updateAssetView(IAssetDescriptor::AssetId assetId, IAssetContainer& container)
    {
        bool needToFabricateAssetView = false;
        IAssetView::Ptr oldAssetView;

        {
            lock_(m_mutex);

            if (needToFabricateAssetView = !m_assetViewCreationState; needToFabricateAssetView)
            {
                m_assetViewCreationState.emplace();
                m_assetViewCreationState.setAutoResetOnReady(true);
                oldAssetView = m_assetViewRef.lock();
                m_assetViewRef = nullptr;
            }
        }

        if (!needToFabricateAssetView)
        {
            co_return;
        }

        IAssetView::Ptr newAssetView = co_await fabricateAssetView(container);

        scope_on_leave
        {
            m_assetViewCreationState.resolve(newAssetView);
        };

        {
            lock_(m_mutex);
            m_assetViewRef = newAssetView;
        }
        if (oldAssetView)
        {
            co_await AssetManagerImpl::getInstance().updateAssetView(assetId, *m_viewType, oldAssetView, newAssetView);
            {
                lock_(m_mutex);
                if (auto reloadableAssetView = m_reloadableAssetViewRef.lock(); reloadableAssetView)
                {
                    reloadableAssetView->reloadAssetView(newAssetView);
                }
            }
        }
    }

    IAssetView::Ptr AssetDescriptorImpl::AssetViewEntry::getFabricatedAssetView()
    {
        lock_(m_mutex);
        return m_assetViewRef.lock();
    }

    AssetDescriptorImpl::~AssetDescriptorImpl()
    {
    }

    AssetDescriptorImpl::AssetDescriptorImpl(AssetPath assetPath, ContainerLoaderFunc loader) :
        m_assetId(AssetManagerImpl::getInstance().getNextAssetId()),
        m_assetPath(std::move(assetPath)),
        m_containerLoader(std::move(loader))
    {
    }

    IAssetDescriptor::Ptr AssetDescriptorImpl::getInnerAsset(eastl::string_view innerPath)
    {
        if (innerPath.empty())
        {
            return this;
        }

        return rtti::createInstance<InnerAssetDescriptor>(this, innerPath);
    }

    IAssetContainer* AssetDescriptorImpl::getLoadedContainer()
    {
        lock_(m_mutex);

        return m_container.get();
    }

    async::Task<IAssetContainer::Ptr> AssetDescriptorImpl::getContainer()
    {
        using namespace nau::async;

        bool needToLoadContainer = false;

        {
            lock_(m_mutex);
            if (m_container)
            {
                co_return m_container;
            }

            if (needToLoadContainer = !m_containerLoadingState; needToLoadContainer)
            {  // first request to the specified asset container
                m_containerLoadingState.emplace();
                m_containerLoadingState.setAutoResetOnReady(true);
            }
        }

        NAU_FATAL(m_containerLoadingState);
        if (needToLoadContainer)
        {  // load container and return it as result
            NAU_FATAL(m_containerLoader);

            Result<IAssetContainer::Ptr> loadContainerResult = co_await m_containerLoader().doTry();
            NAU_ASSERT(!m_container);

            if (!loadContainerResult)
            {
                m_containerLoadingState.resolve(nullptr);
                co_return nullptr;
            }

            m_container = *std::move(loadContainerResult);
            m_containerLoadingState.resolve(m_container);

            if (!m_assetViews.empty())
            {
                eastl::vector<Task<>> updateTasks;
                for (AssetViewEntry& viewEntry : m_assetViews)
                {
                    if (auto task = viewEntry.updateAssetView(m_assetId, *m_container); task && !task.isReady())
                    {
                        updateTasks.emplace_back(std::move(task));
                    }
                }

                co_await async::whenAll(updateTasks);
            }

            co_return m_container;
        }

        // this subsequent request for the container (which is already in loading state): just need to wait for the result of operation.
        IAssetContainer::Ptr container = co_await m_containerLoadingState.getNextTask();
        NAU_ASSERT(container.get() == m_container.get());

        co_return container;
    }

    async::Task<IAssetView::Ptr> AssetDescriptorImpl::getInnerAssetView(eastl::string_view innerPath, const rtti::TypeInfo* viewType)
    {
        auto container = co_await getContainer();
        if (!container)
        {
            NAU_LOG_ERROR("Asset container not loaded. will returns null for view type:({})", viewType ? viewType->getTypeName() : std::string_view{"UNKNOWN"});
            co_return nullptr;
        }

        auto& viewEntry = EXPR_Block->AssetViewEntry&
        {
            lock_(m_mutex);

            auto viewEntryIter = std::find_if(m_assetViews.begin(), m_assetViews.end(), [innerPath, viewType](const AssetViewEntry& viewEntry)
            {
                return viewEntry.isThatView(innerPath, viewType);
            });

            if (viewEntryIter == m_assetViews.end())
            {
                m_assetViews.emplace_back(eastl::string{innerPath}, viewType);
                return m_assetViews.back();
            }

            return *viewEntryIter;
        };

        IAssetView::Ptr assetView = co_await viewEntry.getAssetView(*container);

        co_return assetView;
    }

    async::Task<ReloadableAssetView::Ptr> AssetDescriptorImpl::getInnerReloadableAssetView(eastl::string_view innerPath, const rtti::TypeInfo* viewType)
    {
        auto container = co_await getContainer();
        if (!container)
        {
            NAU_LOG_ERROR("Asset container not loaded. will returns null for view type:({})", viewType ? viewType->getTypeName() : std::string_view{"UNKNOWN"});
            co_return nullptr;
        }

        auto& viewEntry = EXPR_Block->AssetViewEntry&
        {
            lock_(m_mutex);

            auto viewEntryIter = std::find_if(m_assetViews.begin(), m_assetViews.end(), [innerPath, viewType](const AssetViewEntry& viewEntry)
            {
                return viewEntry.isThatView(innerPath, viewType);
            });

            if (viewEntryIter == m_assetViews.end())
            {
                m_assetViews.emplace_back(eastl::string{innerPath}, viewType);
                return m_assetViews.back();
            }

            return *viewEntryIter;
        };

        ReloadableAssetView::Ptr assetView = co_await viewEntry.getReloadableAssetView(*container);

        co_return assetView;
    }

    async::Task<nau::Ptr<>> AssetDescriptorImpl::getInnerRawAsset(eastl::string_view innerPath)
    {
        auto container = co_await getContainer();
        if (!container)
        {
            co_return nullptr;
        }

        co_return container->getAsset(innerPath);
    }

    async::Task<IAssetView::Ptr> AssetDescriptorImpl::getAssetView(const AssetViewDescription& viewDescription)
    {
        return getInnerAssetView({}, viewDescription.viewApi);
    }

    async::Task<ReloadableAssetView::Ptr> AssetDescriptorImpl::getReloadableAssetView(const AssetViewDescription& viewDescription)
    {
        return getInnerReloadableAssetView({}, viewDescription.viewApi);
    }

    async::Task<nau::Ptr<>> AssetDescriptorImpl::getRawAsset()
    {
        return getInnerRawAsset({});
    }

    eastl::optional<assets::AssetInternalState> AssetDescriptorImpl::getInnerCachedAssetViewState(eastl::string_view innerPath, const rtti::TypeInfo* viewType, assets::InternalStateOptsFlag opts)
    {
        if (!viewType)
        {
            NAU_FAILURE("getInnerCachedAssetViewState() currently REQUIRE explicit asset type ");
            return eastl::nullopt;
        }

        lock_(m_mutex);
        if (!m_container)
        {
            return eastl::nullopt;
        }

        auto viewEntryIter = std::find_if(m_assetViews.begin(), m_assetViews.end(), [innerPath, viewType](const AssetViewEntry& viewEntry)
        {
            return viewEntry.isThatView(innerPath, viewType);
        });

        if (viewEntryIter == m_assetViews.end())
        {
            return eastl::nullopt;
        }

        IAssetView::Ptr assetView = viewEntryIter->getFabricatedAssetView();
        if (!assetView)
        {
            return eastl::nullopt;
        }

        nau::Ptr<> assetAccessor;
        if (opts.has(assets::InternalStateOpts::Accessor))
        {
            assetAccessor = m_container->getAsset(innerPath);
        }

        return assets::AssetInternalState{
            .assetId = m_assetId,
            .view = std::move(assetView),
            .accessor = std::move(assetAccessor)};
    }

    eastl::optional<assets::AssetInternalState> AssetDescriptorImpl::getCachedAssetViewInternalState(const rtti::TypeInfo* viewType, assets::InternalStateOptsFlag opts)
    {
        return getInnerCachedAssetViewState("", viewType, opts);
    }

    uint64_t AssetDescriptorImpl::getAssetId() const
    {
        return m_assetId;
    }

    AssetPath AssetDescriptorImpl::getAssetPath() const
    {
        return m_assetPath;
    }

    void AssetDescriptorImpl::load()
    {
        if (lock_(m_mutex); m_container)
        {  // container already loaded - nothing to do.
            return;
        }

        [[maybe_unused]] auto t = getContainer().detach();
    }

    IAssetDescriptor::UnloadResult AssetDescriptorImpl::unload()
    {
        // TODO: final implementation must be ready to handle unload even if container loading is in progress.
        lock_(m_mutex);

        NAU_ASSERT(!m_containerLoadingState || m_containerLoadingState.isReady(), "Unloading asset while it is still loading");

        m_containerLoadingState = nullptr;
        m_container.reset();

        const bool hasNoViewReferences = std::all_of(m_assetViews.begin(), m_assetViews.end(), [](AssetViewEntry& viewEntry)
        {
            return viewEntry.hasNoAssetViewReferences();
        });

        // TODO: think what to do with m_assetViews ?  ( clear() ? )

        AssetUnloaded.post(getBroadcaster(), m_assetId);

        return hasNoViewReferences ? UnloadResult::Unloaded : UnloadResult::UnloadedHasReferences;
    }

    IAssetDescriptor::LoadState AssetDescriptorImpl::getLoadState() const
    {
        lock_(m_mutex);
        if (m_container)
        {
            return LoadState::Ready;
        }

        if (m_containerLoadingState && !m_containerLoadingState.isReady())
        {
            return LoadState::InProgress;
        }

        return LoadState::None;
    }

}  // namespace nau
