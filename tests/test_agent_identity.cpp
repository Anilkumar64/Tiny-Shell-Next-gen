#include "../server/AgentIdentity.h"
#include <gtest/gtest.h>

TEST(AgentIdentityTest, LoadsStableMetadata) {
    const auto first = tsh::AgentIdentity::load_metadata();
    const auto second = tsh::AgentIdentity::load_metadata();

    EXPECT_FALSE(first.agent_id.empty());
    EXPECT_FALSE(first.fingerprint.empty());
    EXPECT_FALSE(first.pairing_code.empty());
    EXPECT_EQ(first.agent_id, second.agent_id);
    EXPECT_EQ(first.pairing_code, second.pairing_code);
}

TEST(AgentIdentityTest, FormatsHumanReadableMetadata) {
    const auto meta = tsh::AgentIdentity::load_metadata();
    const auto text = tsh::AgentIdentity::format_human(meta);

    EXPECT_NE(text.find("Agent ID:"), std::string::npos);
    EXPECT_NE(text.find("Fingerprint:"), std::string::npos);
    EXPECT_NE(text.find("Pairing code:"), std::string::npos);
    EXPECT_NE(text.find("Hostname:"), std::string::npos);
}
