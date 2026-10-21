// Copyright 2026 N-GINN LLC. All rights reserved.

#include <gtest/gtest.h>

#include <cstring>

#if defined(NAU_MGPU_TEST_ENGINE_ALLOCATOR)
    #include "nau/diag/logging.h"
#endif

namespace mgpu_test
{
    int runBlockedShutdownChild();
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
#if defined(NAU_MGPU_TEST_ENGINE_ALLOCATOR)
    // Render's heap provider uses the engine logger; standalone contract tests do not link NauKernel.
    nau::diag::setLogger(nau::diag::createLogger());
#endif
    // A real process exit exercises static destruction with a permanently unsignaled GPU dependency.
    const int result = argc == 2 && std::strcmp(argv[1], "--mgpu-blocked-shutdown-child") == 0
                           ? mgpu_test::runBlockedShutdownChild()
                           : RUN_ALL_TESTS();
#if defined(NAU_MGPU_TEST_ENGINE_ALLOCATOR)
    nau::diag::setLogger(nullptr);
#endif
    return result;
}
