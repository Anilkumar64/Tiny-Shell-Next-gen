#include "Parser.h"
#include <gtest/gtest.h>

TEST(ParserTest, ParsesValidPipeline) {
    auto ast = tsh::Parser::parse_pipeline("ps | filter(cpu > 0) | count()");
    ASSERT_NE(ast, nullptr);
    EXPECT_EQ(ast->name, "ps");
    ASSERT_NE(ast->next, nullptr);
    EXPECT_EQ(ast->next->type, tsh::OpType::FILTER);
}

TEST(ParserTest, RejectsUnlistedCommand) {
    // FIX[M1]: Parser is default-deny and rejects commands outside the allowlist.
    EXPECT_THROW(tsh::Parser::parse_pipeline("rm -rf /"), std::runtime_error);
}
