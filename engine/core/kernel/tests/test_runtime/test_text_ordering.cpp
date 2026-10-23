// Copyright 2026 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#include <gtest/gtest.h>

#include <compare>

#include "nau/string/string.h"

namespace nau::test
{
    TEST(NauStringOrdering, EqualValues)
    {
        const nau::string left = u8"same";
        const nau::string right = u8"same";
        EXPECT_EQ(left <=> right, std::strong_ordering::equal);
    }

    TEST(NauStringOrdering, LexicalOrder)
    {
        const nau::string first = u8"alpha";
        const nau::string second = u8"omega";
        EXPECT_EQ(first <=> second, std::strong_ordering::less);
        EXPECT_EQ(second <=> first, std::strong_ordering::greater);
        EXPECT_LT(first, second);
        EXPECT_GT(second, first);
    }

    TEST(NauStringOrdering, PrefixAndEmptyValues)
    {
        const nau::string empty = u8"";
        const nau::string prefix = u8"a";
        const nau::string longer = u8"ab";
        EXPECT_LT(empty, prefix);
        EXPECT_LT(prefix, longer);
        EXPECT_GT(longer, prefix);
    }

    TEST(NauStringOrdering, MultibyteValues)
    {
        const nau::string first = u8"a\u0431";
        const nau::string second = u8"a\u0432";
        const nau::string copy = first;
        EXPECT_EQ(first <=> second, std::strong_ordering::less);
        EXPECT_EQ(second <=> first, std::strong_ordering::greater);
        EXPECT_EQ(first <=> copy, std::strong_ordering::equal);
    }
}  // namespace nau::test
