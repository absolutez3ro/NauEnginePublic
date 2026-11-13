// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "nau/3d/dag_drv3d.h"
#include "nau/3d/dag_drv3dCmd.h"
#include "nau/diag/logging.h"
#include "render_window_impl.h"

namespace nau::render
{
#if defined(NAU_ENABLE_MGPU)
    // Initial state of the Mgpu path of the render pipeline: the second GPU draws Nau's shadow cascades (see
    // MgpuShadowsState). The value is applied once, when the first render graph is created;
    // nau::mgpu::bridge::setEnabled() switches the path at run time (SceneBaseSample: key M).
    // It only has an effect in a build with NAU_ENABLE_MGPU (preset win_vs2022_x64_mgpu) on a machine with two distinct
    // GPUs, while the directional light casts shadows; with one GPU Nau draws the cascades itself.
    constexpr bool MGPU_SHADOWS_ENABLED = true;

    namespace
    {
        namespace shadows = nau::mgpu::shadows;

        // Matrices go to the second GPU in the memory layout Nau's shaders read them in.
        static_assert(sizeof(math::Matrix4) == 16 * sizeof(float));

        // drv3d_DX12 (render_state.h): D3D12 DepthBias = zBias / MINIMUM_REPRESENTABLE_D16, truncated to INT.
        constexpr double DX12_MINIMUM_REPRESENTABLE_D16 = 2e-5;

        void copyText(char* destination, const size_t capacity, const char* source)
        {
            std::strncpy(destination, source != nullptr ? source : "", capacity - 1);
            destination[capacity - 1] = '\0';
        }

        void appendMatrices(eastl::vector<float>& matrices, const math::Matrix4* source, const size_t count)
        {
            const auto* values = reinterpret_cast<const float*>(source);
            matrices.insert(matrices.end(), values, values + count * 16);
        }

        bool toAtlasFormat(const uint32_t format, shadows::AtlasFormat& out)
        {
            switch (format)
            {
                case TEXFMT_DEPTH16:
                    out = shadows::AtlasFormat::Depth16;
                    return true;
                case TEXFMT_DEPTH32:
                    out = shadows::AtlasFormat::Depth32;
                    return true;
                default:
                    return false;
            }
        }

        bool sameAtlas(const csm::CascadeShadows::AtlasInfo& a, const csm::CascadeShadows::AtlasInfo& b)
        {
            return a.width == b.width && a.height == b.height && a.format == b.format;
        }

        // Read-only lock of a Nau skinned mesh buffer. These buffers are SBCF_DYNAMIC and filled on the CPU once, so this
        // reads their mapped memory without waiting for the GPU. Static meshes are in video memory, their geometry is kept
        // on the CPU (StaticMeshLod).
        class BufferRead
        {
        public:
            explicit BufferRead(Sbuffer* buffer) :
                m_buffer(buffer)
            {
                if (m_buffer != nullptr && !m_buffer->lock(0, 0, &m_data, VBLOCK_READONLY))
                {
                    m_data = nullptr;
                }
            }

            ~BufferRead()
            {
                if (m_data != nullptr)
                {
                    m_buffer->unlock();
                }
            }

            BufferRead(const BufferRead&) = delete;
            BufferRead& operator=(const BufferRead&) = delete;

            const void* data() const
            {
                return m_data;
            }

            int size() const
            {
                return m_buffer != nullptr ? m_buffer->ressize() : 0;
            }

        private:
            Sbuffer* m_buffer;
            void* m_data = nullptr;
        };
    }
#endif

#if defined(NAU_ENABLE_MGPU)
    namespace
    {
        // Stage 4 of the MGPU optimisation (bridge::Settings::nearCascades): the cascades Nau draws itself every frame, the
        // second GPU draws the others. At least one cascade stays on the second GPU (renderMgpuShadows).
        int mgpuNearCascadesSetting()
        {
            return std::min(static_cast<int>(nau::mgpu::bridge::getSettings().nearCascades),
                            static_cast<int>(csm::CascadeShadows::MAX_CASCADES));
        }

        // Stage 7 (bridge::Settings::maxAtlasesPerSecond).
        int mgpuMaxAtlasesSetting()
        {
            return static_cast<int>(nau::mgpu::bridge::getSettings().maxAtlasesPerSecond);
        }
    } // namespace
#endif

    // The Mgpu switch and settings from the "mgpu" section of the application properties, an environment variable
    // overriding a value (for comparisons and debugging): mgpu/enabled (NAU_MGPU, default MGPU_SHADOWS_ENABLED),
    // mgpu/auto (NAU_MGPU_AUTO), mgpu/nearCascades (NAU_MGPU_NEAR_CASCADES), mgpu/maxAtlasesPerSecond
    // (NAU_MGPU_MAX_ATLASES). Applied once; nau::mgpu::bridge::setEnabled / setSettings change them at run time.
    void RenderWindowImpl::initializeMgpu()
    {
#if defined(NAU_ENABLE_MGPU)
        static const bool applied = []
        {
            namespace bridge = nau::mgpu::bridge;
            using namespace settings_detail;

            bool enabled = MGPU_SHADOWS_ENABLED;
            bridge::Settings settings = bridge::getSettings();
            bool autoMode = settings.autoMode;
            int nearCascades = static_cast<int>(settings.nearCascades);
            int maxAtlases = static_cast<int>(settings.maxAtlasesPerSecond);

            GlobalProperties* const properties = findGlobalProperties();
            readProperty(properties, "mgpu/enabled", enabled);
            readProperty(properties, "mgpu/auto", autoMode);
            readProperty(properties, "mgpu/nearCascades", nearCascades);
            readProperty(properties, "mgpu/maxAtlasesPerSecond", maxAtlases);
            readEnvironment("NAU_MGPU", enabled);
            readEnvironment("NAU_MGPU_AUTO", autoMode);
            readEnvironment("NAU_MGPU_NEAR_CASCADES", nearCascades);
            readEnvironment("NAU_MGPU_MAX_ATLASES", maxAtlases);

            settings.autoMode = autoMode;
            settings.nearCascades = static_cast<uint32_t>(std::clamp(nearCascades, 0, static_cast<int>(csm::CascadeShadows::MAX_CASCADES)));
            settings.maxAtlasesPerSecond = static_cast<uint32_t>(std::max(0, maxAtlases));
            bridge::setSettings(settings);
            bridge::setEnabled(enabled);
            NAU_LOG_INFO("mgpu settings: {}, auto {}, {} near cascade(s) on the scene GPU, atlases per second: {}",
                         enabled ? "on" : "off", settings.autoMode ? "on" : "off", settings.nearCascades,
                         settings.maxAtlasesPerSecond > 0 ? ::fmt::format("at most {}", settings.maxAtlasesPerSecond)
                                                          : std::string("no limit"));
            return true;
        }();
        static_cast<void>(applied);
#endif
    }

#if defined(NAU_ENABLE_MGPU)

    RenderWindowImpl::MgpuShadowsState::~MgpuShadowsState()
    {
        if (!releaseEventsCreated)
        {
            return;
        }

        for (size_t slot = 0; slot < releaseEvents.size(); ++slot)
        {
            // The driver signals the event when the frame with the copy has finished; it must not be closed earlier.
            if (releasePending[slot])
            {
                os_event_wait(&releaseEvents[slot], 1000);
            }
            os_event_destroy(&releaseEvents[slot]);
        }
        // The renderer is destroyed after this and waits for the second GPU.
    }

    void RenderWindowImpl::publishMgpuStatus() const
    {
        const auto& state = m_mgpuShadows;
        const shadows::RendererStats& stats = state.renderer.stats();

        nau::mgpu::bridge::Status status;
        status.ready = state.renderer.isReady();
        copyText(status.primaryAdapter, sizeof(status.primaryAdapter), state.renderer.primaryAdapterName());
        copyText(status.secondaryAdapter, sizeof(status.secondaryAdapter), state.renderer.secondaryAdapterName());
        copyText(status.lastError, sizeof(status.lastError), state.renderer.lastError());
        status.sceneFramesPerSecond = state.renderFramesPerSecond;
        status.shadowsFromSecondGpu = state.hasAtlas;
        status.atlasAgeFrames = state.hasAtlas ? static_cast<uint32_t>(state.frame - state.atlasFrame) : 0;
        status.atlasesPerSecond = state.atlasesPerSecond;
        status.latencyMs = stats.lastLatencyMs;
        status.drawCount = stats.lastDrawCount;
        status.atlasesCompleted = stats.completed;
        status.skippedBusy = stats.skippedBusy;
        status.failed = stats.failed + state.copyFailures;
        status.timings = state.timings.published;
        status.autoSuspended = state.autoSwitch.suspended;
        status.autoOtherFramesPerSecond = state.autoSwitch.otherFramesPerSecond;
        nau::mgpu::bridge::publishStatus(status);
    }

    void RenderWindowImpl::beginFrameTiming()
    {
        m_mgpuShadows.timings.gpuFrame.begin();
    }

    void RenderWindowImpl::endFrameTiming()
    {
        auto& timings = m_mgpuShadows.timings;
        timings.gpuFrame.end();
        if (timings.renderStart != std::chrono::steady_clock::time_point{})
        {
            timings.renderSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - timings.renderStart).count();
            ++timings.frames;
        }
    }

    void RenderWindowImpl::reportMgpuTimings()
    {
        using Views = nau::RenderScene::ViewsStats;
        static_assert(sizeof(nau::mgpu::bridge::Status::Timings::instancesPerCascade) / sizeof(uint32_t) == csm::CascadeShadows::MAX_CASCADES);

        auto& state = m_mgpuShadows;
        auto& timings = state.timings;
        nau::mgpu::bridge::Status::Timings& out = timings.published;
        out = {};

        const double frames = std::max<uint32_t>(timings.frames, 1);
        const double listFrames = std::max<uint32_t>(timings.listFrames, 1);
        out.cpuFrameMs = state.renderFramesPerSecond > 0 ? 1000.0f / state.renderFramesPerSecond : 0.0f;
        out.cpuRenderMs = static_cast<float>(timings.renderSeconds * 1000.0 / frames);
        out.cpuListsMainMs = static_cast<float>(timings.listSeconds[Views::Main] * 1000.0 / listFrames);
        out.cpuListsCascadesMs = static_cast<float>(timings.listSeconds[Views::Cascade] * 1000.0 / listFrames);
        out.cpuListsOmniMs = static_cast<float>(timings.listSeconds[Views::Omni] * 1000.0 / listFrames);
        out.instancesMain = static_cast<uint32_t>(timings.listInstances[Views::Main] / listFrames);
        out.instancesCascades = static_cast<uint32_t>(timings.listInstances[Views::Cascade] / listFrames);
        out.instancesOmni = static_cast<uint32_t>(timings.listInstances[Views::Omni] / listFrames);
        for (size_t cascade = 0; cascade < timings.cascadeInstances.size(); ++cascade)
        {
            out.instancesPerCascade[cascade] = static_cast<uint32_t>(timings.cascadeInstances[cascade] / listFrames);
        }
        out.cpuCollectMs = static_cast<float>(timings.collectSeconds * 1000.0 / frames);
        out.cpuSubmitMs = static_cast<float>(timings.submitSeconds * 1000.0 / frames);

        const auto average = [](GpuTimer& timer) -> float
        {
            double milliseconds = 0;
            uint32_t count = 0;
            timer.collect(milliseconds, count);
            return count != 0 ? static_cast<float>(milliseconds / count) : -1.0f;
        };
        out.gpuFrameMs = average(timings.gpuFrame);
        out.gpuNauCascadesMs = average(timings.gpuNauCascades);
        out.gpuAtlasCopyMs = average(timings.gpuAtlasCopy);

        // The renderer sums since its initialization; another initialization starts from zero.
        const shadows::RendererStats& stats = state.renderer.stats();
        if (stats.drawGpuSamples < timings.drawGpuSamples || stats.transferGpuSamples < timings.transferGpuSamples)
        {
            timings.drawGpuMs = 0;
            timings.sharedCopyGpuMs = 0;
            timings.drawGpuSamples = 0;
            timings.transferGpuMs = 0;
            timings.transferGpuSamples = 0;
        }
        if (const uint64_t samples = stats.drawGpuSamples - timings.drawGpuSamples; samples != 0)
        {
            out.gpu2DrawMs = static_cast<float>((stats.drawGpuMsTotal - timings.drawGpuMs) / samples);
            out.gpu2SharedCopyMs = static_cast<float>((stats.sharedCopyGpuMsTotal - timings.sharedCopyGpuMs) / samples);
        }
        if (const uint64_t samples = stats.transferGpuSamples - timings.transferGpuSamples; samples != 0)
        {
            out.gpuTransferMs = static_cast<float>((stats.transferGpuMsTotal - timings.transferGpuMs) / samples);
        }
        timings.drawGpuMs = stats.drawGpuMsTotal;
        timings.sharedCopyGpuMs = stats.sharedCopyGpuMsTotal;
        timings.drawGpuSamples = stats.drawGpuSamples;
        timings.transferGpuMs = stats.transferGpuMsTotal;
        timings.transferGpuSamples = stats.transferGpuSamples;

        const auto ms = [](const float value) -> std::string
        {
            return value < 0 ? std::string("n/a") : ::fmt::format("{:.2f} ms", value);
        };
        NAU_LOG_INFO("mgpu timing: frame {:.2f} ms, render thread {:.2f} ms, scene GPU {}; lists: main {:.2f} ms ({} instances), "
                     "cascades {:.2f} ms ({}: {}/{}/{}/{}/{}/{}), omni {:.2f} ms ({}); second GPU path on the CPU: collect {:.3f} ms, "
                     "submit {:.3f} ms per frame; GPU: Nau cascades {}, atlas copy {}, second GPU draw {}, to shared heap {}, transfer {}",
                     out.cpuFrameMs, out.cpuRenderMs, ms(out.gpuFrameMs), out.cpuListsMainMs, out.instancesMain, out.cpuListsCascadesMs,
                     out.instancesCascades, out.instancesPerCascade[0], out.instancesPerCascade[1], out.instancesPerCascade[2],
                     out.instancesPerCascade[3], out.instancesPerCascade[4], out.instancesPerCascade[5], out.cpuListsOmniMs,
                     out.instancesOmni, out.cpuCollectMs, out.cpuSubmitMs, ms(out.gpuNauCascadesMs), ms(out.gpuAtlasCopyMs),
                     ms(out.gpu2DrawMs), ms(out.gpu2SharedCopyMs), ms(out.gpuTransferMs));

        {
            eastl::array<double, csm::CascadeShadows::MAX_CASCADES> triangles = {};
            for (size_t cascade = 0; cascade < triangles.size(); ++cascade)
            {
                triangles[cascade] = static_cast<double>(timings.cascadeTriangles[cascade]) / listFrames / 1000.0;
            }
            NAU_LOG_INFO("mgpu timing: thousands of caster triangles per cascade: {:.0f}/{:.0f}/{:.0f}/{:.0f}/{:.0f}/{:.0f}; "
                         "cascades drawn by Nau {:.2f} per frame, by the second GPU {:.2f} per atlas; {} frames sent no atlas "
                         "(nothing to draw anew)",
                         triangles[0], triangles[1], triangles[2], triangles[3], triangles[4], triangles[5],
                         static_cast<double>(timings.nauCascadeDraws) / frames,
                         timings.submits != 0 ? static_cast<double>(timings.secondGpuCascadeDraws) / timings.submits : 0.0,
                         timings.unchangedFrames);
        }

        if (state.skippedListFrames != 0 || state.listFallbacks != 0)
        {
            NAU_LOG_INFO("mgpu timing: cascade lists skipped in {} frames (the second GPU busy or the atlas rate reached), rebuilt in {}",
                         state.skippedListFrames, state.listFallbacks);
        }
        state.skippedListFrames = 0;
        state.listFallbacks = 0;

        timings.frames = 0;
        timings.renderSeconds = 0;
        timings.listFrames = 0;
        timings.listSeconds = {};
        timings.listInstances = {};
        timings.cascadeInstances = {};
        timings.cascadeTriangles = {};
        timings.nauCascadeDraws = 0;
        timings.secondGpuCascadeDraws = 0;
        timings.submits = 0;
        timings.unchangedFrames = 0;
        timings.collectSeconds = 0;
        timings.submitSeconds = 0;
    }

    bool RenderWindowImpl::releaseMgpuShadowSlots(MgpuShadowsState& state)
    {
        bool waiting = false;
        for (uint32_t slot = 0; slot < shadows::SLOT_COUNT; ++slot)
        {
            if (!state.releasePending[slot])
            {
                continue;
            }

            if (os_event_wait(&state.releaseEvents[slot], 0) == OS_WAIT_OK)
            {
                state.releasePending[slot] = false;
                state.renderer.release(slot);
            }
            else
            {
                waiting = true;
            }
        }
        return !waiting;
    }

    void RenderWindowImpl::releaseMgpuShadows(MgpuShadowsState& state)
    {
        // Waits for the second GPU. Nau's copies have executed already (releaseMgpuShadowSlots).
        state.renderer.shutdown();
        state.meshes.clear();
        state.pendingFrames.clear();
        state.releasePending = {};
        state.initAttempted = false;
        state.hasAtlas = false;
        state.publishedHasAtlas = false;
        state.reportedFailures = 0;
        state.copyFailures = 0;
        state.atlasesSinceReport = 0;
        state.atlasesPerSecond = 0;
        state.framesSinceReport = 0;
        state.cpuSecondsSinceReport = 0;
    }

    uint64_t RenderWindowImpl::registerMgpuShadowMesh(MgpuShadowsState& state, const RenderEntity& entity)
    {
        // state: the stream the mesh belongs to (every Mgpu renderer has its own meshes).

        const auto bufferKey = eastl::make_pair<const Sbuffer*, const Sbuffer*>(entity.positionBuffer, entity.indexBuffer);
        const int positionsSize = entity.positionBuffer->ressize();
        const int indicesSize = entity.indexBuffer->ressize();

        if (const auto found = state.meshes.find(bufferKey); found != state.meshes.end() &&
                                                             found->second.positionsSize == positionsSize &&
                                                             found->second.indicesSize == indicesSize)
        {
            return found->second.key; // 0: the mesh could not be registered, it is not retried
        }

        // A new mesh, or a buffer address reused by another one: the old mesh stays on the second GPU until the Mgpu
        // path is switched off (CascadeRenderer::shutdown).
        MgpuShadowsState::MeshRecord& record = state.meshes[bufferKey];
        record = {0, positionsSize, indicesSize};

        const StaticMeshLod* const lod = entity.meshLod;
        const bool skinned = lod == nullptr && entity.boneWeightsBuffer != nullptr && entity.boneIndicesBuffer != nullptr;
        const BufferRead positions(lod == nullptr ? entity.positionBuffer : nullptr);
        const BufferRead indices(lod == nullptr ? entity.indexBuffer : nullptr);
        const BufferRead boneWeights(skinned ? entity.boneWeightsBuffer : nullptr);
        const BufferRead boneIndices(skinned ? entity.boneIndicesBuffer : nullptr);

        shadows::MeshDesc mesh;
        mesh.positions = lod != nullptr ? reinterpret_cast<const float*>(lod->m_cpuPositions.data()) : static_cast<const float*>(positions.data());
        mesh.vertexCount = static_cast<uint32_t>(positionsSize / (3 * sizeof(float)));
        mesh.indices = lod != nullptr ? static_cast<const void*>(lod->m_cpuIndices.data()) : indices.data();
        mesh.indices32 = (entity.indexBuffer->getFlags() & SBCF_INDEX32) != 0;
        mesh.indexCount = static_cast<uint32_t>(indicesSize / (mesh.indices32 ? sizeof(uint32_t) : sizeof(uint16_t)));
        if (lod != nullptr)
        {
            mesh.texCoords = reinterpret_cast<const float*>(lod->m_cpuTexCoords.data());
        }
        if (skinned)
        {
            // Nau's skinned vertices: float4 weights + uint4 joints.
            const size_t skinSize = static_cast<size_t>(mesh.vertexCount) * 4 * sizeof(float);
            if (static_cast<size_t>(boneWeights.size()) >= skinSize && static_cast<size_t>(boneIndices.size()) >= skinSize)
            {
                mesh.boneWeights = static_cast<const float*>(boneWeights.data());
                mesh.boneIndices = static_cast<const uint32_t*>(boneIndices.data());
            }
            else
            {
                mesh.positions = nullptr;
            }
        }

        if (mesh.positions == nullptr || mesh.indices == nullptr || (skinned && (mesh.boneWeights == nullptr || mesh.boneIndices == nullptr)))
        {
            NAU_LOG_ERROR("mgpu shadows: could not read a shadow caster mesh ({} vertices, {} indices), Nau draws without it",
                          mesh.vertexCount, mesh.indexCount);
            return 0;
        }

        const uint64_t key = state.nextMeshKey++;
        if (!state.renderer.registerMesh(key, mesh))
        {
            NAU_LOG_ERROR("mgpu shadows: could not register a shadow caster mesh on the second GPU: {}", state.renderer.lastError());
            return 0;
        }

        record.key = key;
        return key;
    }

    uint64_t RenderWindowImpl::registerMgpuShadowMask(MgpuShadowsState& state, const RenderEntity& entity)
    {
        if (!entity.material || entity.meshLod == nullptr || entity.material->getBlendMode("default") != nau::BlendMode::Masked)
        {
            return 0;
        }

        const nau::Ptr<TextureAssetView> view = entity.material->findTextureView("default", "albedoTex");
        if (!view || view->getCpuMips().empty())
        {
            return 0;
        }

        const void* const texture = view.get();
        if (const auto found = state.masks.find(texture); found != state.masks.end())
        {
            return found->second;
        }

        eastl::vector<shadows::MaskMip> mips;
        mips.reserve(view->getCpuMips().size());
        for (const TextureAssetView::CpuMip& mip : view->getCpuMips())
        {
            mips.push_back({mip.data.data(), mip.rowPitch, mip.rowCount});
        }

        shadows::MaskDesc mask;
        mask.width = view->getCpuMipsWidth();
        mask.height = view->getCpuMipsHeight();
        mask.dxgiFormat = view->getCpuMipsDxgiFormat();
        mask.mips = mips.data();
        mask.mipCount = static_cast<uint32_t>(mips.size());

        const uint64_t key = state.nextMaskKey++;
        if (!state.renderer.registerMask(key, mask))
        {
            NAU_LOG_ERROR("mgpu shadows: could not register a shadow mask on the second GPU, the caster is drawn without it: {}", state.renderer.lastError());
            state.masks[texture] = 0;
            return 0;
        }

        state.masks[texture] = key;
        return key;
    }

    // The same cascades and casters Nau draws in renderShadowsCascades (renderCascadeShadowDepth -> RenderView::renderZPrepass
    // of the csmView_<cascade> view), as a frame description for the second GPU. The cascades before firstCascade are
    // described but get no casters: Nau draws them (stage 4).
    void RenderWindowImpl::collectMgpuShadowCasters(const int firstCascade, const int numCascades)
    {
        auto& state = m_mgpuShadows;
        state.cascades.clear();
        state.draws.clear();
        state.matrices.clear();
        state.bones.clear();
        state.skippedCasters = 0;

        for (int c = 0; c < numCascades; ++c)
        {
            shadows::CascadeDesc& cascade = state.cascades.emplace_back();
            std::memcpy(cascade.viewProj, &m_csm->getWorldRenderMatrix(c), sizeof(cascade.viewProj));

            const csm::CascadeShadows::CascadeViewport viewport = m_csm->getCascadeViewport(c);
            cascade.viewport[0] = static_cast<uint32_t>(viewport.x);
            cascade.viewport[1] = static_cast<uint32_t>(viewport.y);
            cascade.viewport[2] = static_cast<uint32_t>(viewport.width);
            cascade.viewport[3] = static_cast<uint32_t>(viewport.height);

            float constBias = 0;
            float slopeBias = 0;
            m_csm->getCascadeDepthBias(c, constBias, slopeBias);
            cascade.depthBias = static_cast<int32_t>(constBias / DX12_MINIMUM_REPRESENTABLE_D16);
            cascade.slopeScaledDepthBias = slopeBias;
        }

        for (auto& view : m_graphicsScene->getRenderScene()->getViews())
        {
            if (!view->containsTag(nau::RenderScene::Tags::shadowCascadeTag))
            {
                continue;
            }
            const int cascade = static_cast<int>(reinterpret_cast<intptr_t>(view->getUserData()));
            if (cascade < firstCascade || cascade >= numCascades)
            {
                continue;
            }

            appendMgpuViewCasters(state, *view, static_cast<uint32_t>(cascade));
        }
    }

    // The shadow casters of one view (a cascade or a face of a point light shadow map) as draws of the frame description.
    void RenderWindowImpl::appendMgpuViewCasters(MgpuShadowsState& state, nau::RenderView& view, const uint32_t cascade)
    {
        // Another scene: a buffer or a texture of the old meshes can be replaced by a new one at the same address.
        if (const uint64_t generation = nau::mgpu::bridge::getMeshGeneration(); generation != state.meshGeneration)
        {
            state.renderer.forgetMeshes();
            state.meshes.clear();
            state.masks.clear();
            state.meshGeneration = generation;
        }

        {
            for (const auto& list : view.getLists())
            {
                for (const RenderEntity& entity : list->getEntities())
                {
                    if (entity.positionBuffer == nullptr || entity.indexBuffer == nullptr || entity.endIndex <= entity.startIndex)
                    {
                        continue;
                    }

                    const uint64_t mesh = registerMgpuShadowMesh(state, entity);
                    if (mesh == 0)
                    {
                        ++state.skippedCasters;
                        continue;
                    }

                    shadows::DrawDesc draw;
                    draw.mesh = mesh;
                    draw.cascade = cascade;
                    draw.startIndex = entity.startIndex;
                    draw.indexCount = (entity.endIndex - entity.startIndex) / 3 * 3;
                    draw.firstMatrix = static_cast<uint32_t>(state.matrices.size() / 16);
                    draw.mask = registerMgpuShadowMask(state, entity);

                    // RenderView::renderZPrepass: instanced entities use the "default" pipeline, the others are skinned
                    // when they have bone buffers.
                    const bool instanced = entity.instancingSupported && entity.instancesCount != 1;
                    const bool skinned = !instanced && entity.boneWeightsBuffer != nullptr && entity.boneIndicesBuffer != nullptr;
                    if (skinned)
                    {
                        const auto bones = entity.cbStructsData.find("BonesTransforms");
                        if (bones == entity.cbStructsData.end() || bones->second.dataPtr == nullptr)
                        {
                            ++state.skippedCasters;
                            continue;
                        }

                        // Bones are already in world space; one copy per skinned instance, shared by its cascades.
                        draw.boneCount = std::min<uint32_t>(static_cast<uint32_t>(bones->second.size / sizeof(math::Matrix4)),
                                                            NAU_MAX_SKINNING_BONES_COUNT);
                        const auto known = std::find_if(state.bones.begin(), state.bones.end(), [&](const auto& item)
                        {
                            return item.first == bones->second.dataPtr;
                        });
                        if (known != state.bones.end())
                        {
                            draw.firstMatrix = known->second;
                        }
                        else
                        {
                            state.bones.emplace_back(bones->second.dataPtr, draw.firstMatrix);
                            appendMatrices(state.matrices, static_cast<const math::Matrix4*>(bones->second.dataPtr), draw.boneCount);
                        }
                    }
                    else
                    {
                        // The z-prepass shader reads instanceBuffer[startInstance + instance].worldMatrix; a non-instanced
                        // draw is instance 0.
                        const uint32_t requested = instanced ? entity.instancesCount : 1;
                        draw.instanceCount = std::min<uint32_t>(requested, static_cast<uint32_t>(entity.instanceData.size()));
                        if (draw.instanceCount == 0)
                        {
                            continue;
                        }
                        for (uint32_t i = 0; i < draw.instanceCount; ++i)
                        {
                            appendMatrices(state.matrices, &entity.instanceData[i].worldMatrix, 1);
                        }
                    }

                    state.draws.push_back(draw);
                }
            }
        }
    }

    bool RenderWindowImpl::checkMgpuStall(MgpuShadowsState& state, const char* what)
    {
        // pendingFrames is in the order of submission; takeReady drops the frames up to the atlas it hands out.
        const bool late = !state.pendingFrames.empty() &&
                          std::chrono::steady_clock::now() - state.pendingFrames.front().submitted >
                              std::chrono::duration<float>(MGPU_STALL_SECONDS) &&
                          state.frame - state.pendingFrames.front().frame >= MGPU_STALL_FRAMES;
        const bool stalled = late || state.failedSubmitsInRow >= MGPU_FAILED_SUBMITS_LIMIT;
        if (stalled != state.stalled)
        {
            state.stalled = stalled;
            if (stalled)
            {
                NAU_LOG_WARNING("{}: the second GPU has stopped delivering atlases ({}), Nau draws the shadows itself until it "
                                "does again: {}", what, late ? "an atlas is late by over a second" : "its submits fail",
                                state.renderer.lastError());
            }
            else
            {
                NAU_LOG_INFO("{}: the second GPU delivers atlases again", what);
            }
        }
        if (stalled)
        {
            state.hasAtlas = false;
        }
        return stalled;
    }

    void RenderWindowImpl::noteMgpuSubmit(MgpuShadowsState& state, const uint64_t failedBefore, const bool submitted)
    {
        if (state.renderer.stats().failed > failedBefore)
        {
            ++state.failedSubmitsInRow;
        }
        else if (submitted)
        {
            state.failedSubmitsInRow = 0;
        }
    }

    bool RenderWindowImpl::mgpuSubmitDue(const MgpuShadowsState& state)
    {
        return mgpuMaxAtlasesSetting() <= 0 || std::chrono::steady_clock::now() >= state.nextSubmit;
    }

    void RenderWindowImpl::mgpuSubmitted(MgpuShadowsState& state)
    {
        const int rate = mgpuMaxAtlasesSetting();
        if (rate <= 0)
        {
            return;
        }
        // A steady rate: the next time follows the previous one, unless that one is more than a period ago.
        const auto now = std::chrono::steady_clock::now();
        const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / rate));
        state.nextSubmit = state.nextSubmit + period > now ? state.nextSubmit + period : now + period / 2;
    }

    void RenderWindowImpl::updateMgpuAutoSwitch()
    {
        constexpr float FIRST_PROBE_SECONDS = 10.0f; // after a start or another scene
        constexpr float PROBE_SECONDS = 30.0f;
        constexpr float MAX_PROBE_SECONDS = 240.0f; // the interval doubles after every try that kept the choice
        constexpr float FASTER = 1.05f;

        auto& state = m_mgpuShadows;
        auto& a = state.autoSwitch;
        const auto now = std::chrono::steady_clock::now();
        const auto after = [now](const float seconds)
        {
            return now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(seconds));
        };

        if (!nau::mgpu::bridge::isAutoEnabled() || !nau::mgpu::bridge::isEnabled() || !state.renderer.isReady())
        {
            if (a.suspended || a.probing)
            {
                NAU_LOG_INFO("mgpu auto: off, the second GPU draws the shadows while Mgpu is on");
            }
            a = {};
            return;
        }

        // Another scene, or the first period of the mode: the next try comes soon.
        const uint64_t generation = nau::mgpu::bridge::getMeshGeneration();
        if (a.nextProbe == std::chrono::steady_clock::time_point{} || generation != a.meshGeneration)
        {
            a.meshGeneration = generation;
            a.intervalSeconds = PROBE_SECONDS;
            a.nextProbe = after(FIRST_PROBE_SECONDS);
            if (!a.probing)
            {
                return;
            }
        }

        const float fps = state.renderFramesPerSecond;
        if (!a.probing)
        {
            a.framesPerSecond = fps;
            if (now >= a.nextProbe)
            {
                a.probing = true;
                a.probePeriods = 0;
                a.suspended = !a.suspended;
                NAU_LOG_INFO("mgpu auto: trying {} ({:.1f} fps now)", a.suspended ? "one GPU" : "the second GPU", fps);
            }
            return;
        }

        // The first period after a switch settles: the second GPU sends its first atlas, the lists fill up.
        if (++a.probePeriods < 2)
        {
            return;
        }
        a.probing = false;
        if (fps > a.framesPerSecond * FASTER)
        {
            a.otherFramesPerSecond = a.framesPerSecond;
            a.intervalSeconds = PROBE_SECONDS;
            NAU_LOG_INFO("mgpu auto: {} kept, {:.1f} fps against {:.1f}", a.suspended ? "one GPU" : "the second GPU", fps,
                         a.framesPerSecond);
            a.framesPerSecond = fps;
        }
        else
        {
            a.otherFramesPerSecond = fps;
            a.suspended = !a.suspended;
            a.intervalSeconds = std::min(a.intervalSeconds * 2.0f, MAX_PROBE_SECONDS);
            NAU_LOG_INFO("mgpu auto: back to {}, {:.1f} fps against {:.1f} tried", a.suspended ? "one GPU" : "the second GPU",
                         a.framesPerSecond, fps);
        }
        a.nextProbe = after(a.intervalSeconds);
    }

    bool RenderWindowImpl::suspendMgpuRenderer(MgpuShadowsState& state)
    {
        releaseMgpuShadowSlots(state);
        shadows::ReadyAtlas ready;
        if (state.renderer.takeReady(ready))
        {
            state.renderer.release(ready.slot); // takeReady has dropped the older ones
        }
        state.pendingFrames.clear();
        state.hasAtlas = false;
        state.nearCascades = 0;
        state.fullRedraw = true; // Nau's cascades go on meanwhile, the persistent atlas of the second GPU gets old
        return false;
    }

    void RenderWindowImpl::renderMgpuNearCascades()
    {
        auto& state = m_mgpuShadows;
        if (state.nearCascades <= 0)
        {
            return;
        }
        state.timings.nauCascadeDraws += static_cast<uint64_t>(state.nearCascades);
        state.timings.gpuNauCascades.begin();
        m_csm->renderNearShadowCascades(state.nearCascades);
        state.timings.gpuNauCascades.end();
    }

    void RenderWindowImpl::updateMgpuResolveSnapshot()
    {
        auto& state = m_mgpuShadows;
        state.resolveSnapshot = state.atlasSnapshot;
        if (!state.hasAtlas)
        {
            return;
        }

        // The far cascades: drawn by the second GPU with the matrices of an older frame, which are relative to the camera of
        // that frame, while the resolve measures from the camera of this one (worldViewPos - worldPos). Moved to this camera
        // the way CascadeShadows moves a kept cascade: the shadows stay where they were drawn instead of following the camera
        // by the distance it went during the latency. In the shader t = p.x*col0 + p.y*col1 + p.z*col2 + col3, with p from
        // the camera; from the old camera p is d farther, d = the new camera - the old one.
        auto& tm = state.resolveSnapshot.constants.shadowCascadeTmTransp;
        for (int cascade = state.nearCascades; cascade < state.atlasSnapshot.numCascades; ++cascade)
        {
            const math::Vector3 d = m_csm->getRenderCameraWorldViewPos(cascade) - state.atlasViewPos[cascade];
            const float dx = d.getX();
            const float dy = d.getY();
            const float dz = d.getZ();
            math::Color4& col3 = tm[cascade * 4 + 3];
            const math::Color4& col0 = tm[cascade * 4 + 0];
            const math::Color4& col1 = tm[cascade * 4 + 1];
            const math::Color4& col2 = tm[cascade * 4 + 2];
            col3.r += dx * col0.r + dy * col1.r + dz * col2.r;
            col3.g += dx * col0.g + dy * col1.g + dz * col2.g;
            col3.b += dx * col0.b + dy * col1.b + dz * col2.b;
        }

        // The near cascades Nau has drawn this frame: the constants of this frame.
        if (state.nearCascades <= 0)
        {
            return;
        }
        const csm::CascadeShadows::ShaderSnapshot current = m_csm->snapshotShaderConstants();
        for (int cascade = 0; cascade < state.nearCascades; ++cascade)
        {
            for (int row = 0; row < 4; ++row)
            {
                state.resolveSnapshot.constants.shadowCascadeTmTransp[cascade * 4 + row] =
                    current.constants.shadowCascadeTmTransp[cascade * 4 + row];
            }
            state.resolveSnapshot.constants.shadowCascadeTcMulOffset[cascade] = current.constants.shadowCascadeTcMulOffset[cascade];
        }
    }

    bool RenderWindowImpl::renderMgpuShadows()
    {
        auto& state = m_mgpuShadows;
        ++state.frame;

        // On the render thread, between the commands of a frame.
        if (const bool vsync = nau::mgpu::bridge::isVsyncEnabled(); vsync != state.vsyncApplied)
        {
            d3d::enable_vsync(vsync);
            state.vsyncApplied = vsync;
        }

        // Render frame rate for the UI, in every mode.
        const auto frameTime = std::chrono::steady_clock::now();
        if (state.renderFrames++ == 0)
        {
            state.renderFramesStart = frameTime;
        }
        else if (const float elapsed = std::chrono::duration<float>(frameTime - state.renderFramesStart).count();
                 elapsed >= MGPU_REPORT_PERIOD_SECONDS)
        {
            state.renderFramesPerSecond = static_cast<float>(state.renderFrames - 1) / elapsed;
            state.renderFrames = 1;
            state.renderFramesStart = frameTime;
            reportMgpuTimings();
            updateMgpuAutoSwitch();
            publishMgpuStatus();
            NAU_LOG_INFO("mgpu frame rate: {:.1f} fps, {:.2f} ms (cascades: {}, near cascades on the scene GPU: {}, vsync {})",
                         state.renderFramesPerSecond, 1000.0f / std::max(state.renderFramesPerSecond, 0.001f),
                         state.hasAtlas ? "second GPU" : "Nau", state.hasAtlas ? state.nearCascades : 0,
                         state.vsyncApplied ? "on" : "off");
        }

        state.nearCascades = 0; // set below once the cascades of this frame are known
        if (!nau::mgpu::bridge::isEnabled())
        {
            state.hasAtlas = false;
            // Shut down once Nau's copies from the shared heaps have executed; until then nothing new is started.
            if (state.initAttempted && releaseMgpuShadowSlots(state))
            {
                releaseMgpuShadows(state);
                publishMgpuStatus();
                NAU_LOG_INFO("mgpu shadows: switched off, Nau draws the shadow cascades");
            }
            return false;
        }

        if (!state.initAttempted)
        {
            state.initAttempted = true;
            state.lastReport = std::chrono::steady_clock::now();

            if (!state.releaseEventsCreated)
            {
                for (os_event_t& event : state.releaseEvents)
                {
                    os_event_create(&event);
                }
                state.releaseEventsCreated = true;
            }

            state.fullRedraw = true;
            // The primary Mgpu device is the one on the adapter Nau renders on: the shared heaps are copied from there.
            if (state.renderer.initialize(d3d::get_device_name()))
            {
                NAU_LOG_INFO("mgpu shadows: the shadow cascades are drawn by '{}', the scene by '{}'",
                             state.renderer.secondaryAdapterName(), state.renderer.primaryAdapterName());
            }
            else
            {
                NAU_LOG_WARNING("mgpu shadows: second GPU not available, Nau draws the shadow cascades: {}", state.renderer.lastError());
            }
            publishMgpuStatus();
        }

        if (!state.renderer.isReady())
        {
            state.hasAtlas = false; // the resolve must not take the constants of an old atlas for Nau's cascades
            return false;
        }
        if (state.autoSwitch.suspended)
        {
            return suspendMgpuRenderer(state);
        }

        const auto workStart = std::chrono::steady_clock::now();
        ++state.framesSinceReport;
        releaseMgpuShadowSlots(state);

        const int numCascades = m_csm->getNumCascadesToRender();
        const csm::CascadeShadows::AtlasInfo atlas = m_csm->getAtlasInfo();
        shadows::AtlasFormat format = shadows::AtlasFormat::Depth16;
        if (numCascades <= 0 || !toAtlasFormat(atlas.format, format))
        {
            // No shadows this frame: Nau clears its atlas.
            state.hasAtlas = false;
            return false;
        }
        if (state.hasAtlas && (!sameAtlas(state.atlasSnapshot.atlas, atlas) || state.atlasSnapshot.numCascades != numCascades))
        {
            state.hasAtlas = false; // Nau's atlas was recreated (another size or format), or the cascades are laid out anew
        }
        state.nearCascades = std::min(mgpuNearCascadesSetting(), numCascades - 1);
        if (sparseShadowFrames() > 0)
        {
            // The near cascades are never kept (FAR_SHADOW_CASCADE): Nau draws them over the atlas every frame.
            state.nearCascades = std::min(state.nearCascades, FAR_SHADOW_CASCADE);
        }

        // Stage 5: what the persistent atlas of the second GPU has to draw anew.
        if (!sameAtlas(state.redrawAtlas, atlas) || state.redrawNumCascades != numCascades ||
            nau::mgpu::bridge::getMeshGeneration() != state.meshGeneration ||
            state.renderer.stats().failed != state.reportedFailures)
        {
            state.fullRedraw = true;
            state.redrawAtlas = atlas;
            state.redrawNumCascades = numCascades;
        }
        for (int cascade = 0; cascade < numCascades; ++cascade)
        {
            state.redrawPending[cascade] = state.redrawPending[cascade] || m_csm->shouldUpdateCascade(cascade);
        }

        // 1. The newest atlas the second GPU has finished goes into Nau's atlas, with the constants it was drawn with.
        shadows::ReadyAtlas ready;
        if (state.renderer.takeReady(ready))
        {
            const auto pending = std::find_if(state.pendingFrames.begin(), state.pendingFrames.end(), [&](const auto& item)
            {
                return item.token == ready.userToken;
            });

            bool copied = false;
            if (pending != state.pendingFrames.end() && sameAtlas(pending->snapshot.atlas, atlas) &&
                pending->snapshot.numCascades == numCascades)
            {
                BaseTexture* target = m_csm->getShadowsCascade();
                state.timings.gpuAtlasCopy.begin();
                copied = d3d::driver_command(DRV3D_COMMAND_D3D12_COPY_FROM_EXTERNAL_RESOURCE, ready.resource, target, nullptr) != 0;
                state.timings.gpuAtlasCopy.end();
                if (copied)
                {
                    // Same state Nau leaves its atlas in after drawing it (CascadeShadows::renderShadowsCascadesCb).
                    d3d::resource_barrier({target, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0});
                    d3d::driver_command(DRV3D_COMMAND_REGISTER_FRAME_COMPLETION_EVENT, &state.releaseEvents[ready.slot], nullptr, nullptr);
                    state.releasePending[ready.slot] = true;

                    state.hasAtlas = true;
                    state.secondGpuCascadesInAtlas = true;
                    state.atlasSnapshot = pending->snapshot;
                    state.atlasViewPos = pending->viewPos;
                    state.atlasFrame = pending->frame;
                    ++state.atlasesSinceReport;
                }
                else if (state.copyFailures++ == 0)
                {
                    NAU_LOG_ERROR("mgpu shadows: the driver did not copy the atlas of the second GPU, Nau draws the shadow cascades");
                }
            }

            if (!copied)
            {
                // Drawn for an atlas Nau has recreated since, or refused by the driver: the slot is free right away.
                state.renderer.release(ready.slot);
            }

            // takeReady has dropped the older finished atlases.
            state.pendingFrames.erase(std::remove_if(state.pendingFrames.begin(), state.pendingFrames.end(), [&](const auto& item)
            {
                return item.token <= ready.userToken;
            }), state.pendingFrames.end());
        }

        checkMgpuStall(state, "mgpu shadows");

        // 2. The cascades of this frame go to the second GPU if it has a free slot; it never waits.
        // Without this frame's cascade lists (skipped while the second GPU was busy or the rate reached) nothing is
        // submitted: the casters would be missing from the atlas.
        updateMgpuResolveSnapshot();
        if (state.cascadeListsSkipped || !mgpuSubmitDue(state))
        {
            return state.hasAtlas;
        }

        const auto collectStart = std::chrono::steady_clock::now();
        const auto redraw = [&state](const int cascade)
        {
            return cascade >= state.nearCascades && (state.fullRedraw || state.redrawPending[cascade]);
        };
        // Nothing to draw anew (every far cascade is kept): no atlas is sent, Nau's atlas or the one on its way holds the
        // far cascades already. In a light scene the transfer over the bus is the dearest part of an atlas.
        bool anyRedraw = false;
        for (int cascade = 0; cascade < numCascades; ++cascade)
        {
            anyRedraw = anyRedraw || redraw(cascade);
        }
        if (!anyRedraw && (state.hasAtlas || !state.pendingFrames.empty()))
        {
            ++state.timings.unchangedFrames;
            return state.hasAtlas;
        }
        buildCascadeLists(redraw);
        collectMgpuShadowCasters(state.nearCascades, numCascades);
        uint32_t redrawn = 0;
        for (int cascade = 0; cascade < numCascades; ++cascade)
        {
            // A near cascade is cleared (Nau draws it over the atlas), a far one drawn anew unless the atlas keeps it.
            state.cascades[cascade].keep = cascade >= state.nearCascades && !redraw(cascade);
            redrawn += redraw(cascade) ? 1 : 0;
        }
        const auto submitStart = std::chrono::steady_clock::now();
        state.timings.collectSeconds += std::chrono::duration<double>(submitStart - collectStart).count();

        shadows::FrameDesc frame;
        frame.atlasWidth = static_cast<uint32_t>(atlas.width);
        frame.atlasHeight = static_cast<uint32_t>(atlas.height);
        frame.format = format;
        frame.cascades = state.cascades.data();
        frame.cascadeCount = static_cast<uint32_t>(state.cascades.size());
        frame.draws = state.draws.data();
        frame.drawCount = static_cast<uint32_t>(state.draws.size());
        frame.matrices = state.matrices.data();
        frame.matrixCount = static_cast<uint32_t>(state.matrices.size() / 16);
        frame.userToken = state.frame;
        const uint64_t failedBefore = state.renderer.stats().failed;
        const bool submitted = state.renderer.submit(frame);
        noteMgpuSubmit(state, failedBefore, submitted);
        state.timings.submitSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - submitStart).count();
        if (submitted)
        {
            MgpuShadowsState::PendingFrame& pending = state.pendingFrames.push_back();
            pending.token = state.frame;
            pending.frame = state.frame;
            pending.snapshot = m_csm->snapshotShaderConstants();
            pending.submitted = std::chrono::steady_clock::now();
            for (int cascade = 0; cascade < numCascades; ++cascade)
            {
                pending.viewPos[cascade] = m_csm->getRenderCameraWorldViewPos(cascade);
            }
            state.redrawPending = {};
            state.fullRedraw = false;
            mgpuSubmitted(state);
            state.timings.secondGpuCascadeDraws += redrawn;
            ++state.timings.submits;
        }

        // 3. Log and UI.
        const auto now = std::chrono::steady_clock::now();
        state.cpuSecondsSinceReport += std::chrono::duration<double>(now - workStart).count();

        const shadows::RendererStats& stats = state.renderer.stats();
        if (stats.failed != state.reportedFailures)
        {
            NAU_LOG_ERROR("mgpu shadows: {} atlas(es) FAILED on the second GPU: {}", stats.failed - state.reportedFailures,
                          state.renderer.lastError());
            state.reportedFailures = stats.failed;
        }

        const float elapsed = std::chrono::duration<float>(now - state.lastReport).count();
        if (elapsed >= MGPU_REPORT_PERIOD_SECONDS)
        {
            state.atlasesPerSecond = static_cast<float>(state.atlasesSinceReport) / elapsed;
            const float framesPerSecond = static_cast<float>(state.framesSinceReport) / elapsed;
            const double cpuMsPerFrame = state.framesSinceReport != 0 ? state.cpuSecondsSinceReport * 1000.0 / state.framesSinceReport : 0.0;
            state.atlasesSinceReport = 0;
            state.framesSinceReport = 0;
            state.cpuSecondsSinceReport = 0;
            state.lastReport = now;
            publishMgpuStatus();
            state.publishedHasAtlas = state.hasAtlas;

            NAU_LOG_INFO("mgpu shadows: {:.1f} atlases/s from '{}', {} draws ({} casters skipped), latency {:.1f} ms "
                         "(transfer {:.1f} ms), atlas age {} frames, {} completed, {} skipped (busy), {} failed; scene {:.1f} fps, "
                         "{:.3f} ms CPU per frame",
                         state.atlasesPerSecond, state.renderer.secondaryAdapterName(), stats.lastDrawCount, state.skippedCasters,
                         stats.lastLatencyMs, stats.lastTransferMs,
                         state.hasAtlas ? state.frame - state.atlasFrame : 0, stats.completed, stats.skippedBusy,
                         stats.failed + state.copyFailures, framesPerSecond, cpuMsPerFrame);
        }
        else if (state.publishedHasAtlas != state.hasAtlas)
        {
            publishMgpuStatus();
            state.publishedHasAtlas = state.hasAtlas;
        }

        return state.hasAtlas;
    }

#endif
}  // namespace nau::render
