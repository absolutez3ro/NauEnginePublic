// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#pragma once

#include <EASTL/unordered_set.h>

#include "nau/assets/asset_ref.h"
#include "nau/assets/asset_view.h"
#include "nau/assets/material.h"
#include "nau/async/task_base.h"
#include "nau/rtti/rtti_impl.h"

#include "shader_asset.h"
#include "texture_asset.h"

namespace nau
{
    /**
     * @brief Structure for creating a buffer (SRV or UAV).
     *
     * This structure holds the information necessary to create a buffer,
     * whether it is a Shader Resource View (SRV) or an Unordered Access View (UAV).
     */
    struct BufferDesc
    {
        const char8_t* name;    ///< Debug name of the buffer (currently not functional).
        int elementSize;        ///< Size of a single element (or structure). 
        int elementCount;       ///< Number of elements in the buffer. 
        unsigned flags;         ///< Additional flags. @warning Under no circumstances should `TEXFMT` be included in the flags, as this implicitly makes all buffers structured! 
        unsigned format;        ///< The data format (`TEXFMT`) of the buffer. 
    };


    /**
     * @brief Structure for creating a read-write texture.
     *
     * This structure holds the information necessary to create a read-write texture,
     * including its dimensions, image data, and additional properties.
     */
    struct TextureDesc
    {
        const char8_t* name;    ///< Debug name of the texture.
        TexImage32* image;      ///< Pointer to the texture image data.
        int width;              ///< Width of the texture.
        int height;             ///< Height of the texture.
        int depthOrArraySize;   ///< Depth of the texture or size of the texture array.
        int flags;              ///< Additional flags for texture creation, including `TEXFMT` format.
        int levels;             ///< Number of mipmap levels in the texture.
    };


    /**
     * @brief Interface and base class for material asset views, implementing shared logic for master and instance material classes.
     *
     * This class serves as both an interface and a partial implementation, encapsulating common functionality
     * used by both master material classes and material instance classes. It provides a unified approach
     * to handling material assets and simplifies code reuse across various material types.
     */
    class NAU_GRAPHICSASSETS_EXPORT MaterialAssetView : public IAssetView
    {
        NAU_CLASS_(nau::MaterialAssetView, IAssetView)
        using Ptr = nau::Ptr<MaterialAssetView>;

    public:
        /**
         * @brief Asynchronously creates a material asset view from the given accessor.
         * 
         * @param [in] accessor Provides data for constructing the view.
         * @return              Task with operation status and access to the created view.
         */
        static async::Task<MaterialAssetView::Ptr> createFromAssetAccessor(nau::Ptr<> accessor);

        /**
         * @brief Binds the resource for use.
         */
        virtual void bind() = 0;

        /**
         * @brief Binds the specified pipeline for use.
         * 
         * @param [in] pipelineName The name of the pipeline to bind.
         */
        virtual void bindPipeline(eastl::string_view pipelineName) = 0;

        /**
         * @brief Retrieves the program associated with the specified pipeline.
         * 
         * @param [in] pipelineName The name of the pipeline.
         * @return                  The program associated with the pipeline.
         */
        virtual PROGRAM getPipelineProgram(eastl::string_view pipelineName) const = 0;

        /**
         * @brief Retrieves a set of all pipeline names.
         * 
         * @return A set containing the names of all pipelines.
         */
        virtual eastl::unordered_set<eastl::string> getPipelineNames() const;

        bool hasPipeline(eastl::string_view pipelineName) const;

        /**
         * @brief Sets the cull mode for the specified pipeline.
         * 
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] cullMode     The cull mode to set.
         */
        virtual void setCullMode(eastl::string_view pipelineName, CullMode cullMode);

        /**
         * @brief Gets the cull mode for the specified pipeline.
         * 
         * @param [in] pipelineName The name of the pipeline.
         * @return                  The cull mode of the pipeline.
         */
        virtual CullMode getCullMode(eastl::string_view pipelineName) const;

        /**
         * @brief Sets the depth mode for the specified pipeline.
         * 
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] depthMode    The depth mode to set.
         */
        virtual void setDepthMode(eastl::string_view pipelineName, DepthMode depthMode);
        
        /**
         * @brief Gets the depth mode for the specified pipeline.
         * 
         * @param [in] pipelineName The name of the pipeline.
         * @return                  The depth mode of the pipeline.
         */
        virtual DepthMode getDepthMode(eastl::string_view pipelineName) const;

        /**
         * @brief Sets the blend mode for the specified pipeline.
         * 
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] blendMode    The blend mode to set.
         */
        virtual void setBlendMode(eastl::string_view pipelineName, BlendMode blendMode);
        
        /**
         * @brief Gets the blend mode for the specified pipeline.
         * 
         * @param [in] pipelineName The name of the pipeline.
         * @return                  The blend mode of the pipeline.
         */
        virtual BlendMode getBlendMode(eastl::string_view pipelineName) const;

        /**
         * @brief Enables or disables the scissor test for the specified pipeline.
         * 
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] isEnabled    `true` to enable the scissor test, `false` to disable it.
         */
        virtual void setScissorsEnabled(eastl::string_view pipelineName, bool isEnabled);

        /**
         * @brief Checks if the scissor test is enabled for the specified pipeline.
         * 
         * @param [in] pipelineName The name of the pipeline.
         * @return                  `true` if the scissor test is enabled, `false` otherwise.
         */
        virtual bool isScissorsEnabled(eastl::string_view pipelineName) const;

        /**
         * @brief Retrieves the name.
         * 
         * @return The name as a string.
         */
        eastl::string getName() const { return m_name; }

        size_t getNameHash() const { return m_nameHash; }

        /**
         * @brief Enables or disables automatic texture setting.
         * 
         * @param [in] isEnabled `true` to enable automatic texture setting, `false` to disable it.
         */
        void enableAutoSetTextures(bool isEnabled) { m_autoSetTextures = isEnabled; }

        /**
         * @brief Overrides a pixel-shader sampler after automatic material bindings.
         * @param pipelineName The material pipeline.
         * @param samplerName The sampler name declared in HLSL.
         * @param sampler The sampler handle to bind.
         * @param slot The HLSL sampler register.
         */
        void setSampler(eastl::string_view pipelineName, eastl::string_view samplerName, d3d::SamplerHandle sampler, uint32_t slot);

        /**
         * @brief Checks if automatic texture setting is enabled.
         * 
         * @return `true` if automatic texture setting is enabled, `false` otherwise.
         */
        bool isAutoSetTexturesEnabled() const { return m_autoSetTextures; }

        /**
         * @brief Sets a property for a specified pipeline.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] propertyName The name of the property to be set.
         * @param [in] value        The new value to set for the property.
         */
        template <typename T>
        void setProperty(eastl::string_view pipelineName, eastl::string_view propertyName, const T& value);

        /**
         * @brief Retrieves a property value for a specified pipeline.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] propertyName The name of the property to retrieve.
         * @return                  The value of the property, cast to the specified type `T`.
         */
        template <typename T>
        T getProperty(eastl::string_view pipelineName, eastl::string_view propertyName);

        /**
         * @brief Sets a constant buffer for a specified pipeline.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] bufferName   The name of the constant buffer.
         * @param [in] cbuffer      A pointer to the constant buffer to set.
         */
        void setCBuffer(eastl::string_view pipelineName, eastl::string_view bufferName, Sbuffer* cbuffer);

        /**
         * @brief Retrieves a constant buffer for a specified pipeline.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] bufferName   The name of the constant buffer to retrieve.
         * @return                  A pointer to the constant buffer.
         */
        Sbuffer* getCBuffer(eastl::string_view pipelineName, eastl::string_view bufferName);

        /**
         * @brief Sets a texture property for a specified pipeline.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] propertyName The name of the texture property to set.
         * @param [in] texture      A pointer to the texture to assign.
         */
        void setTexture(eastl::string_view pipelineName, eastl::string_view propertyName, BaseTexture* texture);

        /// Returns the current texture, including inherited material instance values.
        virtual BaseTexture* getTexture(eastl::string_view pipelineName, eastl::string_view propertyName);

        /**
         * @brief Sets a solid color texture for a pipeline property.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] propertyName The name of the texture property.
         * @param [in] color        The color to set for the texture (in RGBA format).
         */
        void setSolidColorTexture(eastl::string_view pipelineName, eastl::string_view propertyName, math::E3DCOLOR color);

        /**
         * @brief Sets a texture for a pipeline property from an asset.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] propertyName The name of the texture property.
         * @param [in] textureAsset The asset reference for the texture.
         */
        async::Task<> setTextureFromAsset(eastl::string_view pipelineName, eastl::string_view propertyName, eastl::string_view textureAsset);

        /**
         * @brief Creates a read-write buffer for a specified pipeline.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] bufferName   The name of the buffer to create.
         * @param [in] desc         The buffer description.
         */
        void createRwBuffer(eastl::string_view pipelineName, eastl::string_view bufferName, const BufferDesc& desc);

        /**
         * @brief Writes data to a read-write buffer in a specified pipeline.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] bufferName   The name of the buffer to write to.
         * @param [in] data         The data to write to the buffer.
         * @param [in] size         The size of the data to write.
         */
        void writeRwBuffer(eastl::string_view pipelineName, eastl::string_view bufferName, const void* data, size_t size);
        
        /**
         * @brief Reads data from a read-write buffer in a specified pipeline.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] bufferName   The name of the buffer to read from.
         * @param [out] data        The memory where the data will be copied.
         * @param [in] size         The size of the data to read.
         */
        void readRwBuffer(eastl::string_view pipelineName, eastl::string_view bufferName, void* data, size_t size);
        
        /**
         * @brief Sets a read-write buffer for a specified pipeline.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] bufferName   The name of the buffer to set.
         * @param [in] rwBuffer     The buffer to assign to the pipeline.
         */
        void setRwBuffer(eastl::string_view pipelineName, eastl::string_view bufferName, Sbuffer* rwBuffer);
        
        /**
         * @brief Retrieves a read-write buffer from a specified pipeline.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] bufferName   The name of the buffer to retrieve.
         * @return The read-write buffer if found.
         */
        Sbuffer* getRwBuffer(eastl::string_view pipelineName, eastl::string_view bufferName);
        
        /**
         * @brief Creates a read-only buffer for a specified pipeline.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] bufferName   The name of the buffer to create.
         * @param [in] desc         The buffer description.
         */
        void createRoBuffer(eastl::string_view pipelineName, eastl::string_view bufferName, const BufferDesc& desc);
        
        /**
         * @brief Writes data to a read-only buffer in a specified pipeline.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] bufferName   The name of the buffer to write to.
         * @param [in] data         The data to write to the buffer.
         * @param [in] size         The size of the data to write.
         */
        void writeRoBuffer(eastl::string_view pipelineName, eastl::string_view bufferName, const void* data, size_t size);

        /**
         * @brief Sets a read-only buffer for a specified pipeline.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] bufferName   The name of the buffer to set.
         * @param [in] rwBuffer     The buffer to assign to the pipeline.
         */
        void setRoBuffer(eastl::string_view pipelineName, eastl::string_view bufferName, Sbuffer* roBuffer);

        /**
         * @brief Retrieves a read-only buffer from a specified pipeline.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] bufferName   The name of the buffer to retrieve.
         * @return The read-only buffer if found.
         */
        Sbuffer* getRoBuffer(eastl::string_view pipelineName, eastl::string_view bufferName);
        
        /**
         * @brief Creates a read-write texture for a specified pipeline.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] bufferName   The name of the texture to create.
         * @param [in] desc         The texture description.
         */
        void createRwTexture(eastl::string_view pipelineName, eastl::string_view bufferName, const TextureDesc& desc);

        /**
         * @brief Writes data to a read-write texture in a specified pipeline.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] bufferName   The name of the texture to write to.
         * @param [in] data         The data to write to the texture.
         * @param [in] size         The size of the data to write.
         */
        void writeRwTexture(eastl::string_view pipelineName, eastl::string_view bufferName, const void* data, size_t size);

        /**
         * @brief Reads data from a read-write texture in a specified pipeline.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] bufferName   The name of the texture to read from.
         * @param [out] data        The memory where the data will be copied.
         * @param [in] size         The size of the data to read.
         */
        void readRwTexture(eastl::string_view pipelineName, eastl::string_view bufferName, void* data, size_t size);

        /**
         * @brief Sets a read-write texture for a specified pipeline.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] bufferName   The name of the texture to set.
         * @param [in] rwTexture    The texture to assign to the pipeline.
         */
        void setRwTexture(eastl::string_view pipelineName, eastl::string_view bufferName, BaseTexture* rwTexture);

        /**
         * @brief Retrieves a read-write texture from a specified pipeline.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] bufferName   The name of the texture to retrieve.
         * @return The read-write texture if found.
         */
        BaseTexture* getRwTexture(eastl::string_view pipelineName, eastl::string_view bufferName);

        /**
         * @brief Creates a read-only texture for a specified pipeline.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] bufferName   The name of the texture to create.
         * @param [in] desc         The texture description.
         */
        void createRoTexture(eastl::string_view pipelineName, eastl::string_view bufferName, const TextureDesc& desc);

        /**
         * @brief Writes data to a read-only texture in a specified pipeline.
         *
         * Note: Writing to a read-only texture may not be allowed, depending on the pipeline configuration.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] bufferName   The name of the texture to write to.
         * @param [in] data         The data to write to the texture.
         * @param [in] size         The size of the data to write.
         */
        void writeRoTexture(eastl::string_view pipelineName, eastl::string_view bufferName, const void* data, size_t size);

        /**
         * @brief Sets a read-only texture for a specified pipeline.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] bufferName   The name of the texture to set.
         * @param [in] roTexture    The texture to assign to the pipeline.
         */
        void setRoTexture(eastl::string_view pipelineName, eastl::string_view bufferName, BaseTexture* roTexture);

        /**
         * @brief Retrieves a read-only texture from a specified pipeline.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @param [in] bufferName   The name of the texture to retrieve.
         * @return The read-only texture if found.
         */
        BaseTexture* getRoTexture(eastl::string_view pipelineName, eastl::string_view bufferName);

        /**
         * @brief Dispatches a compute shader workload to a compute pipeline.
         *
         * @param [in] threadGroupCountX The number of thread groups to dispatch in the X dimension.
         * @param [in] threadGroupCountY The number of thread groups to dispatch in the Y dimension.
         * @param [in] threadGroupCountZ The number of thread groups to dispatch in the Z dimension.
         */
        void dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ);

    protected:
        using Timestamp = std::chrono::time_point<std::chrono::steady_clock>;
        
        /**
         * @brief Represents a cached buffer resource and manages its bindings across various pipeline stages.
         */
        struct BufferCache
        {
            eastl::unordered_set<ShaderStage> stages;
            const ShaderInputBindDescription* reflection;
            Sbuffer* buffer;
            uint32_t slot;
            bool isOwned;
            bool isDirty;
        };

        /**
         * @brief Represents a cached texture resource and manages its bindings across various pipeline stages.
         */
        struct TextureCache
        {
            eastl::unordered_set<ShaderStage> stages;
            ReloadableAssetView::Ptr textureView;         ///< Caches the texture asset persistently to prevent repeated loading.
            BaseTexture* texture = nullptr;
            uint32_t slot;
            bool isOwned;

            BaseTexture* getTexture() const
            {
                if (textureView == nullptr)
                {
                    return texture;
                }
                else
                {
                    nau::Ptr<TextureAssetView> textureViewPtr;
                    textureView->getTyped<TextureAssetView>(textureViewPtr);
                    return textureViewPtr->getTexture();
                }
            };
        };

        /**
         * @brief Represents a cached sampler resource and manages its bindings across various pipeline stages.
         */
        struct SamplerCache
        {
            eastl::unordered_set<ShaderStage> stages;
            d3d::SamplerHandle handle;
            uint32_t slot;
        };

        /**
         * @brief Represents a variable within a constant buffer, including its reflection data and current value.
         */
        struct ConstantBufferVariable
        {
            const ShaderVariableDescription* reflection;
            BufferCache* parentBuffer;

            RuntimeValue::Ptr currentValue;
            RuntimeValue::Ptr* masterValue; ///< Only for MaterialInstanceView.

            Timestamp timestamp;

            bool isMasterValue; ///< Only for MaterialInstanceView.
        };

        /**
         * @brief Represents a property of a sampled texture, including its current and master values.
         */
        struct SampledTextureProperty
        {
            TextureCache* parentTexture;

            RuntimeValue::Ptr currentValue;
            RuntimeValue::Ptr* masterValue; ///< Only for MaterialInstanceView.

            Timestamp timestamp;

            bool isMasterValue; ///< Only for MaterialInstanceView.
        };

        /**
         * @brief Descriptor for a render pipeline pass.
         *
         * This structure encapsulates the inputs for a render pipeline pass, including shaders, resources, and other state-related properties.
         * It is similar to a pipeline state object, such as those used in Direct3D 12 (e.g.,
         * `Pipeline State <https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-creategraphicspipelinestate>`).
         * The pipeline stores various resources such as constant buffers, textures, and samplers, as well as configuration options
         * like render state, culling mode, and depth/blend settings.
         */
        struct Pipeline
        {
            eastl::vector<ShaderAssetView::Ptr> shaders;
            
            eastl::unordered_map<eastl::string, ConstantBufferVariable> properties;
            eastl::unordered_map<eastl::string, SampledTextureProperty> texProperties;

            eastl::unordered_map<eastl::string, BufferCache> constantBuffers;
            eastl::unordered_map<eastl::string, BufferCache> systemCBuffers;
            
            eastl::unordered_map<eastl::string, BufferCache> rwBuffers;
            eastl::unordered_map<eastl::string, BufferCache> roBuffers;
            
            eastl::unordered_map<eastl::string, TextureCache> rwTextures;
            eastl::unordered_map<eastl::string, TextureCache> roTextures;
            
            eastl::unordered_map<eastl::string, TextureCache> samplerTextures;
            eastl::unordered_map<eastl::string, SamplerCache> samplers;
            eastl::unordered_map<eastl::string, SamplerCache> samplerOverrides;
            
            PROGRAM programID;

            eastl::optional<shaders::RenderStateId> renderStateId;

            eastl::optional<CullMode> cullMode;
            eastl::optional<DepthMode> depthMode;
            eastl::optional<BlendMode> blendMode;
            eastl::optional<bool> isScissorsEnabled;
            eastl::optional<ComparisonFunc> stencilCmpFunc;

            bool isDirty;
            bool isRenderStateDirty;
        };

        /**
         * @brief Constructs a master render pipeline based on the provided material pipeline and shader data.
         *
         * @param [in] pipelineName     The name to assign to the pipeline.
         * @param [in] materialPipeline The compiled material pipeline, which includes material-specific properties.
         * @param [in] shaders          A span of shader asset views to bind to the pipeline.
         * @return                      A task that provides the constructed pipeline and its operation status once complete.
         */
        static async::Task<Pipeline> makeMasterPipeline(eastl::string_view pipelineName, const MaterialPipeline& materialPipeline, eastl::span<ShaderAssetView::Ptr> shaders);
        
        /**
         * @brief Constructs an instance of a render pipeline based on a master pipeline and material properties.
         *
         * @param [in] pipelineName     The name to assign to the pipeline.
         * @param [in] materialPipeline The material-specific pipeline data that overrides values in the master pipeline.
         * @param [in] masterPipeline   The master pipeline containing the base properties, textures, buffers, and other resources.
         * @return                      A task that provides the constructed pipeline instance once complete.
         */
        static async::Task<Pipeline> makeInstancePipeline(eastl::string_view pipelineName, const MaterialPipeline& materialPipeline, Pipeline& masterPipeline);
        
        /**
         * @brief Sets the culling mode for the given render state.
         *
         * @param [in] cullMode         The desired culling mode (None, Clockwise, CounterClockwise).
         * @param [in, out] renderState The render state object to which the culling mode will be applied.
         */
        static void makeCullMode(CullMode cullMode, shaders::RenderState& renderState);

        /**
         * @brief Sets the depth mode for the given render state.
         *
         * @param [in] depthMode        The desired depth mode (Default, ReadOnly, WriteOnly, Disabled).
         * @param [in, out] renderState The render state object to which the depth mode will be applied.
         */
        static void makeDepthMode(DepthMode depthMode, shaders::RenderState& renderState);

        /**
         * @brief Sets the blend mode for the given render state.
         *
         * @param [in] blendMode        The desired blend mode.
         * @param [in, out] renderState The render state object to which the blend mode will be applied.
         */
        static void makeBlendMode(BlendMode blendMode, shaders::RenderState& renderState);

        /**
         * @brief Sets the stencil comparison function for the given render state.
         *
         * @param [in] cmpFunc          The comparison function to use for the stencil test.
         * @param [in, out] renderState The render state object to which the stencil comparison function will be applied.
         */
        static void makeStencilCmpFunc(ComparisonFunc cmpFunc, shaders::RenderState& renderState);

        /**
         * @brief Updates the constant buffers bound to the pipeline with the associated CPU values.
         *
         * @param pipelineName [in] The name of the pipeline for which constant buffers are updated.
         */
        void updateBuffers(eastl::string_view pipelineName);

        /**
         * @brief Updates the render state for the specified pipeline based on the pipeline's settings.
         *
         * @param pipelineName [in] The name of the pipeline for which the render state is updated.
         */
        void updateRenderState(eastl::string_view pipelineName);

        /**
         * @brief Checks if any of the pipelines have a compute shader.
         *
         * @return True if any pipeline contains a compute shader, false otherwise.
         */
        bool hasComputeShader() const;

        // Map storing pipeline objects by their names.
        eastl::unordered_map<eastl::string, Pipeline> m_pipelines;

        // The name associated with this material asset view.
        eastl::string m_name;

        // Hash of the material name
        size_t m_nameHash;

        // Flag indicating whether textures should be automatically set.
        bool m_autoSetTextures = true;
    };


    /**
     * @brief Encapsulates the master material view, extending the functionality of MaterialAssetView.
     *
     * This class represents a master material, providing additional functionality specific to master materials.
     */
    class NAU_GRAPHICSASSETS_EXPORT MasterMaterialAssetView : public MaterialAssetView
    {
        NAU_CLASS_(nau::MasterMaterialAssetView, MaterialAssetView)
        using Ptr = nau::Ptr<MasterMaterialAssetView>;
        friend class MaterialInstanceAssetView;

    public:
        ~MasterMaterialAssetView() override;

        /**
         * @brief Asynchronously creates a master material asset view from the given material object.
         *
         * @param [in] material The material object to construct the view from.
         * @return              Task with operation status and access to the created master material view.
         */
        static async::Task<MasterMaterialAssetView::Ptr> createFromMaterial(Material&& material);

        /**
         * @brief Binds the default pipeline for the material view.
         */
        void bind() override;

        /**
         * @brief Binds the specified pipeline for use in rendering.
         *
         * @param [in] pipelineName The name of the pipeline to bind.
         */
        void bindPipeline(eastl::string_view pipelineName) override;

        /**
         * @brief Retrieves the program associated with the specified pipeline.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @return                  The program associated with the pipeline.
         */
        PROGRAM getPipelineProgram(eastl::string_view pipelineName) const override;

    private:
        // TODO(MaxWolf): remove this in NAU-2398.
        void setGlobals(eastl::string_view pipelineName);

        // Stores the name of the default program associated with the first pipeline.
        eastl::string m_defaultProgram;
    };


    /**
     * @brief Encapsulates material instance view.
     *
     * This class represents the view for a material instance, providing functionality specific 
     * to instances of materials rather than the master materials.
     */
    class NAU_GRAPHICSASSETS_EXPORT MaterialInstanceAssetView : public MaterialAssetView
    {
        NAU_CLASS_(nau::MaterialInstanceAssetView, MaterialAssetView)
        using Ptr = nau::Ptr<MaterialInstanceAssetView>;

    public:
        ~MaterialInstanceAssetView() override;

        /**
         * @brief Constructs the view from the material instance object.
         *
         * @param [in] material Material instance object to construct the view from.
         * @return              Task object providing operation status as well as the constructed view.
         *
         * @warning `material.master` must reference a master material handle.
         */
        static async::Task<MaterialInstanceAssetView::Ptr> createFromMaterial(Material&& material);

        /**
         * @brief Binds the default pipeline for the material view.
         */
        void bind() override;

        /**
         * @brief Binds the specified pipeline for use in rendering.
         *
         * @param [in] pipelineName The name of the pipeline to bind.
         */
        void bindPipeline(eastl::string_view pipelineName) override;

        BaseTexture* getTexture(eastl::string_view pipelineName, eastl::string_view propertyName) override;

        /**
         * @brief Retrieves the program associated with the specified pipeline from the master material.
         *
         * @param [in] pipelineName The name of the pipeline.
         * @return                  The program associated with the pipeline from the master material.
         *
         * @note The instance does not own the program; it references the program from the master material.
         */
        PROGRAM getPipelineProgram(eastl::string_view pipelineName) const override;

    private:
        void syncBuffers(const Pipeline& masterPipeline, Pipeline& instancePipeline);
        void syncTextures(const Pipeline& masterPipeline, Pipeline& instancePipeline);

        MasterMaterialAssetView::Ptr m_masterMaterial;
    };


    template <typename T>
    void MaterialAssetView::setProperty(eastl::string_view pipelineName, eastl::string_view propertyName, const T& value)
    {
        NAU_ASSERT(m_pipelines.contains(pipelineName));
        auto& pipeline = m_pipelines[pipelineName.data()];

        NAU_ASSERT(pipeline.properties.contains(propertyName));
        auto& variable = pipeline.properties[propertyName.data()];

        if (variable.isMasterValue)
        {
            variable.masterValue = nullptr;
            variable.isMasterValue = false;
        }

        variable.currentValue = makeValueCopy(value);
        variable.timestamp = std::chrono::steady_clock::now();
        variable.parentBuffer->isDirty = true;

        pipeline.isDirty = true;
    }

    template <typename T>
    T MaterialAssetView::getProperty(eastl::string_view pipelineName, eastl::string_view propertyName)
    {
        NAU_ASSERT(m_pipelines.contains(pipelineName));
        auto& pipeline = m_pipelines[pipelineName.data()];

        NAU_ASSERT(pipeline.properties.contains(propertyName));
        auto& variable = pipeline.properties[propertyName.data()];

        return variable.isMasterValue
            ? runtimeValueCast<T>(*variable.masterValue)
            : runtimeValueCast<T>(variable.currentValue);
    }
}  // namespace nau
