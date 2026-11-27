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
    constexpr bool MGPU_SHADOWS_ENABLED = true;

    namespace
    {
        namespace shadows = nau::mgpu::shadows;

        static_assert(sizeof(math::Matrix4) == 16 * sizeof(float));

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

        bool toAtlasFormat(const uint32_t format, shadows::AtlasFormat& atlasFormat)
        {
            switch (format)
            {
                case TEXFMT_DEPTH16:
                    atlasFormat = shadows::AtlasFormat::Depth16;
                    return true;
                case TEXFMT_DEPTH32:
                    atlasFormat = shadows::AtlasFormat::Depth32;
                    return true;
                default:
                    return false;
            }
        }

        bool sameAtlas(const csm::CascadeShadows::AtlasInfo& lhs, const csm::CascadeShadows::AtlasInfo& rhs)
        {
            return lhs.width == rhs.width && lhs.height == rhs.height && lhs.format == rhs.format;
        }

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
    }  // namespace
#endif

#if defined(NAU_ENABLE_MGPU)
    namespace
    {
        int mgpuNearCascadesSetting()
        {
            return std::min(static_cast<int>(nau::mgpu::bridge::getSettings().nearCascades),
                            static_cast<int>(csm::CascadeShadows::MAX_CASCADES));
        }

        int mgpuMaxAtlasesSetting()
        {
            return static_cast<int>(nau::mgpu::bridge::getSettings().maxAtlasesPerSecond);
        }
    }  // namespace
#endif

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
            if (releasePending[slot])
            {
                os_event_wait(&releaseEvents[slot], 1000);
            }
            os_event_destroy(&releaseEvents[slot]);
        }
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

    void RenderWindowImpl::recordFrameTiming() const
    {
        const auto& shadows = m_mgpuShadows;
        nau::mgpu::bridge::recordFrameTiming(shadows.hasAtlas, m_mgpuOmniShadows.hasAtlas,
                                             shadows.hasAtlas ? static_cast<uint32_t>(shadows.frame - shadows.atlasFrame) : 0);
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
        NAU_LOG_INFO(
            "mgpu timing: frame {:.2f} ms, render thread {:.2f} ms, scene GPU {}; lists: main {:.2f} ms ({} instances), "
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
            NAU_LOG_INFO(
                "mgpu timing: thousands of caster triangles per cascade: {:.0f}/{:.0f}/{:.0f}/{:.0f}/{:.0f}/{:.0f}; "
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
        const auto bufferKey = eastl::make_pair<const Sbuffer*, const Sbuffer*>(entity.positionBuffer, entity.indexBuffer);
        const int positionsSize = entity.positionBuffer->ressize();
        const int indicesSize = entity.indexBuffer->ressize();

        const auto found = state.meshes.find(bufferKey);
        const bool known = found != state.meshes.end() && found->second.positionsSize == positionsSize &&
                           found->second.indicesSize == indicesSize;
        if (known && found->second.key != 0)
        {
            return found->second.key;
        }

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
            if (!known)
            {
                NAU_LOG_ERROR("mgpu shadows: could not read a shadow caster mesh ({} vertices, {} indices), will retry",
                              mesh.vertexCount, mesh.indexCount);
            }
            return 0;
        }

        const uint64_t key = state.nextMeshKey++;
        if (!state.renderer.registerMesh(key, mesh))
        {
            if (!known)
            {
                NAU_LOG_ERROR("mgpu shadows: could not register a shadow caster mesh on the second GPU, will retry: {}", state.renderer.lastError());
            }
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
        const auto found = state.masks.find(texture);
        if (found != state.masks.end() && found->second != 0)
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
            if (found == state.masks.end())
            {
                NAU_LOG_ERROR("mgpu shadows: could not register a shadow mask on the second GPU, will retry: {}", state.renderer.lastError());
            }
            state.masks[texture] = 0;
            return 0;
        }

        state.masks[texture] = key;
        return key;
    }

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

    void RenderWindowImpl::appendMgpuViewCasters(MgpuShadowsState& state, nau::RenderView& view, const uint32_t cascade)
    {
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
                NAU_LOG_WARNING(
                    "{}: the second GPU has stopped delivering atlases ({}), Nau draws the shadows itself until it "
                    "does again: {}",
                    what, late ? "an atlas is late by over a second" : "its submits fail",
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
        const auto now = std::chrono::steady_clock::now();
        const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / rate));
        state.nextSubmit = state.nextSubmit + period > now ? state.nextSubmit + period : now + period / 2;
    }

    void RenderWindowImpl::updateMgpuAutoSwitch()
    {
        constexpr float FIRST_PROBE_SECONDS = 10.0f;
        constexpr float PROBE_SECONDS = 30.0f;
        constexpr float MAX_PROBE_SECONDS = 240.0f;
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
            state.renderer.release(ready.slot);
        }
        state.pendingFrames.clear();
        state.hasAtlas = false;
        state.nearCascades = 0;
        state.fullRedraw = true;
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

        if (const bool vsync = nau::mgpu::bridge::isVsyncEnabled(); vsync != state.vsyncApplied)
        {
            d3d::enable_vsync(vsync);
            state.vsyncApplied = vsync;
        }

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

        state.nearCascades = 0;
        if (!nau::mgpu::bridge::isEnabled())
        {
            state.hasAtlas = false;
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
            state.hasAtlas = false;
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
            state.hasAtlas = false;
            return false;
        }
        if (state.hasAtlas && (!sameAtlas(state.atlasSnapshot.atlas, atlas) || state.atlasSnapshot.numCascades != numCascades))
        {
            state.hasAtlas = false;
        }
        state.nearCascades = std::min(mgpuNearCascadesSetting(), numCascades - 1);
        if (sparseShadowFrames() > 0)
        {
            state.nearCascades = std::min(state.nearCascades, FAR_SHADOW_CASCADE);
        }

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
                state.renderer.release(ready.slot);
            }

            state.pendingFrames.erase(std::remove_if(state.pendingFrames.begin(), state.pendingFrames.end(), [&](const auto& item)
            {
                return item.token <= ready.userToken;
            }),
                                      state.pendingFrames.end());
        }

        checkMgpuStall(state, "mgpu shadows");

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

            NAU_LOG_INFO(
                "mgpu shadows: {:.1f} atlases/s from '{}', {} draws ({} casters skipped), latency {:.1f} ms "
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
