#include "AstSigner.h"
#include <cstdlib>
#include <gtest/gtest.h>

TEST(AstSignerTest, SignVerifyRoundTrip) {
    setenv("TSH_SECRET_KEY", "01234567890123456789012345678901", 1);
    const std::vector<uint8_t> payload = {'p', 's'};
    const auto sig = tsh::AstSigner::sign(payload);
    EXPECT_TRUE(tsh::AstSigner::verify(payload, sig));
}

TEST(AstSignerTest, RejectsTamperedPayload) {
    setenv("TSH_SECRET_KEY", "01234567890123456789012345678901", 1);
    const std::vector<uint8_t> payload = {'p', 's'};
    auto sig = tsh::AstSigner::sign(payload);
    const std::vector<uint8_t> tampered = {'p', 'x'};
    EXPECT_FALSE(tsh::AstSigner::verify(tampered, sig));
}
