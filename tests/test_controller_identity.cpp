#include "../common/ControllerIdentity.h"
#include "../server/ControllerTrust.h"
#include <gtest/gtest.h>

TEST(ControllerIdentityTest, LoadsStableControllerId) {
    const auto first = tsh::ControllerIdentity::load_or_create_id();
    const auto second = tsh::ControllerIdentity::load_or_create_id();

    EXPECT_FALSE(first.empty());
    EXPECT_EQ(first, second);
}

TEST(ControllerTrustTest, StoresTrustedController) {
    const std::string id = "ctrl-test-registration";
    tsh::ControllerTrust::trust(id);

    EXPECT_TRUE(tsh::ControllerTrust::is_trusted(id));
}
