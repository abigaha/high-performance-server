#include "authorization.h"

#include <gtest/gtest.h>

#include <chrono>

namespace hps {
namespace {

using Clock = std::chrono::system_clock;

User make_user(UserRole role, std::optional<Clock::time_point> expires_at = std::nullopt) {
  User user;
  user.user_id = 42;
  user.username = "member";
  user.role = role;
  user.vip_expires_at = expires_at;
  return user;
}

TEST(AuthorizationTest, UsesExplicitCapabilitiesForAllFourRoles) {
  const auto now = Clock::time_point{std::chrono::seconds{1'000}};

  const auto guest = make_effective_identity(make_user(UserRole::GUEST), now);
  const auto normal = make_effective_identity(make_user(UserRole::NORMAL), now);
  const auto vip = make_effective_identity(make_user(UserRole::VIP, now + std::chrono::seconds{1}), now);
  const auto admin = make_effective_identity(make_user(UserRole::ADMIN), now);

  EXPECT_FALSE(has_capability(guest, Capability::USE_AUTHENTICATED_FEATURES));
  EXPECT_TRUE(has_capability(normal, Capability::USE_AUTHENTICATED_FEATURES));
  EXPECT_FALSE(has_capability(normal, Capability::USE_VIP_BENEFITS));
  EXPECT_TRUE(has_capability(vip, Capability::USE_AUTHENTICATED_FEATURES));
  EXPECT_TRUE(has_capability(vip, Capability::USE_VIP_BENEFITS));
  EXPECT_FALSE(has_capability(vip, Capability::MANAGE_USERS));
  EXPECT_TRUE(has_capability(admin, Capability::USE_AUTHENTICATED_FEATURES));
  EXPECT_TRUE(has_capability(admin, Capability::MANAGE_USERS));
  EXPECT_TRUE(has_capability(admin, Capability::DELETE_ANY_FILE));
  EXPECT_FALSE(has_capability(admin, Capability::USE_VIP_BENEFITS));
}

TEST(AuthorizationTest, VipIsEffectiveOnlyStrictlyBeforeExpiration) {
  const auto now = Clock::time_point{std::chrono::seconds{1'000}};

  EXPECT_TRUE(is_effective_vip(make_user(UserRole::VIP, now + std::chrono::seconds{1}), now));
  EXPECT_FALSE(is_effective_vip(make_user(UserRole::VIP, now), now));
  EXPECT_FALSE(is_effective_vip(make_user(UserRole::VIP, now - std::chrono::seconds{1}), now));
  EXPECT_FALSE(is_effective_vip(make_user(UserRole::NORMAL, now + std::chrono::seconds{1}), now));
}

TEST(AuthorizationTest, VipExpiresAtTheExactMicrosecondBoundary) {
  const auto expires_at = Clock::time_point{std::chrono::seconds{1'000}} + std::chrono::microseconds{123'456};

  EXPECT_TRUE(is_effective_vip(make_user(UserRole::VIP, expires_at), expires_at - std::chrono::microseconds{1}));
  EXPECT_FALSE(is_effective_vip(make_user(UserRole::VIP, expires_at), expires_at));
  EXPECT_FALSE(is_effective_vip(make_user(UserRole::VIP, expires_at), expires_at + std::chrono::microseconds{1}));
}

TEST(AuthorizationTest, ExpiredVipHasNormalEffectiveRoleAndExpiredStatus) {
  const auto now = Clock::time_point{std::chrono::seconds{1'000}};
  const auto expires_at = now - std::chrono::seconds{1};

  const auto identity = make_effective_identity(make_user(UserRole::VIP, expires_at), now);

  EXPECT_EQ(identity.role, UserRole::NORMAL);
  EXPECT_EQ(identity.vip_status, VipStatus::EXPIRED);
  EXPECT_EQ(identity.vip_expires_at, expires_at);
}

TEST(AuthorizationTest, ContradictoryRoleExpiryStatesFailClosed) {
  const auto now = Clock::time_point{std::chrono::seconds{1'000}};
  const std::array invalid_users = {
    make_user(UserRole::GUEST, now + std::chrono::seconds{1}),
    make_user(UserRole::NORMAL, now + std::chrono::seconds{1}),
    make_user(UserRole::ADMIN, now + std::chrono::seconds{1}),
    make_user(UserRole::VIP),
  };

  for (const auto& user : invalid_users) {
    const auto identity = make_effective_identity(user, now);

    EXPECT_EQ(identity.role, UserRole::GUEST);
    EXPECT_EQ(identity.vip_status, VipStatus::NONE);
    EXPECT_FALSE(identity.vip_expires_at.has_value());
    EXPECT_FALSE(has_capability(identity, Capability::USE_AUTHENTICATED_FEATURES));
  }
}

TEST(AuthorizationTest, UsesNormalUploadLimitForAdmin) {
  ServerConfig config;
  config.normal_max_size = 10;
  config.vip_max_size = 100;

  EXPECT_EQ(role_size_limit(UserRole::GUEST, config), 0U);
  EXPECT_EQ(role_size_limit(UserRole::NORMAL, config), 10U);
  EXPECT_EQ(role_size_limit(UserRole::VIP, config), 100U);
  EXPECT_EQ(role_size_limit(UserRole::ADMIN, config), 10U);
}

} // namespace
} // namespace hps

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
