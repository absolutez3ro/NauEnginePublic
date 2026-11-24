// ao_algorithms.cpp
//
// Copyright 2026 N-GINN LLC. All rights reserved.
#include "ao_algorithm.h"
namespace nau::render::ao
{
    eastl::unique_ptr<Algorithm> createAlgorithm(AmbientOcclusionAlgorithm algorithm)
    {
        switch (algorithm)
        {
            case AmbientOcclusionAlgorithm::hbao:
                return createHbao();
            case AmbientOcclusionAlgorithm::xegtao:
                return createXeGtao();
            default:
                return createSsao();
        }
    }
}  // namespace nau::render::ao
