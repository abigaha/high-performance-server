#include "authorization.h"

namespace hps {

bool is_effective_vip(const User& user, std::chrono::system_clock::time_point now) noexcept {
  return has_valid_vip_expiry_state(user) && user.role == UserRole::VIP && *user.vip_expires_at > now;
}

UserRole effective_role(const User& user, std::chrono::system_clock::time_point now) noexcept {
  if (!has_valid_vip_expiry_state(user)) {
    return UserRole::GUEST;
  }
  if (user.role == UserRole::VIP && !is_effective_vip(user, now)) {
    return UserRole::NORMAL;
  }
  return user.role;
}

EffectiveIdentity make_effective_identity(const User& user, std::chrono::system_clock::time_point now) noexcept {
  EffectiveIdentity identity;
  identity.user_id = user.user_id;
  identity.username = user.username;
  if (!has_valid_vip_expiry_state(user)) {
    return identity;
  }
  identity.role = effective_role(user, now);
  identity.vip_expires_at = user.vip_expires_at;
  if (is_effective_vip(user, now)) {
    identity.vip_status = VipStatus::ACTIVE;
  } else if (user.role == UserRole::VIP && user.vip_expires_at.has_value()) {
    identity.vip_status = VipStatus::EXPIRED;
  }
  return identity;
}

std::size_t role_size_limit(UserRole role, const ServerConfig& config) noexcept {
  switch (role) {
    case UserRole::VIP:
      return static_cast<std::size_t>(config.vip_max_size);
    case UserRole::NORMAL:
    case UserRole::ADMIN:
      return static_cast<std::size_t>(config.normal_max_size);
    case UserRole::GUEST:
      return 0;
  }
  return 0;
}

} // namespace hps
