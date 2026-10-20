// Copyright 2026 N-GINN LLC. All rights reserved.

#include <gtest/gtest.h>

#if defined(NAU_MGPU_TEST_ENGINE_ALLOCATOR)
    #include "nau/diag/logging.h"
#endif

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
#if defined(NAU_MGPU_TEST_ENGINE_ALLOCATOR)
    // Render's heap provider uses the engine logger; standalone contract tests do not link NauKernel.
    nau::diag::setLogger(nau::diag::createLogger());
#endif
    const int result = RUN_ALL_TESTS();
#if defined(NAU_MGPU_TEST_ENGINE_ALLOCATOR)
    nau::diag::setLogger(nullptr);
#endif
    return result;
}
