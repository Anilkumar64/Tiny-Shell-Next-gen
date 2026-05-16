#include "ProcReader.h"
#include <gtest/gtest.h>

TEST(ProcReaderTest, ReadsAtLeastOneProcess) {
    const auto processes = tsh::ProcReader::read_all_processes();
    EXPECT_FALSE(processes.empty());
}
