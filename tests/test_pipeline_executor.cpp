#include "../common/Parser.h"
#include "../server/Pipeline.h"
#include <gtest/gtest.h>

TEST(PipelineExecutorTest, ExecutesUptimeCommand) {
  const auto ast = tsh::Parser::parse_pipeline("uptime");
  const auto output = tsh::Executor::execute(ast);

  EXPECT_NE(output.find("load average"), std::string::npos);
  EXPECT_EQ(output.find("Unknown command or unsupported source"),
            std::string::npos);
}

TEST(PipelineExecutorTest, ExecutesWhoCommand) {
  const auto ast = tsh::Parser::parse_pipeline("who");
  const auto output = tsh::Executor::execute(ast);

  EXPECT_EQ(output.find("Unknown command or unsupported source"),
            std::string::npos);
}

TEST(PipelineExecutorTest, ExecutesDfCommand) {
  const auto ast = tsh::Parser::parse_pipeline("df");
  const auto output = tsh::Executor::execute(ast);

  EXPECT_NE(output.find("Filesystem"), std::string::npos);
  EXPECT_EQ(output.find("Unknown command or unsupported source"),
            std::string::npos);
}

TEST(PipelineExecutorTest, RejectsUnknownCommand) {
  EXPECT_THROW(tsh::Parser::parse_pipeline("hello"), std::runtime_error);
}
