#pragma once

#include "main_functions.h"
#include "models.h"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace hps {

enum class Capability : uint8_t {
  USE_AUTHENTICATED_FEATURES,
  USE_VIP_BENEFITS,
  MANAGE_USERS,
  DELETE_ANY_FILE,
};

bool is_effective_vip(const User& user, std::chrono::system_clock::time_point now) noexcept;
UserRole effective_role(const User& user, std::chrono::system_clock::time_point now) noexcept;
EffectiveIdentity make_effective_identity(const User& user, std::chrono::system_clock::time_point now) noexcept;

inline bool has_capability(const EffectiveIdentity& identity, Capability capability) noexcept {
  switch (capability) {
    case Capability::USE_AUTHENTICATED_FEATURES:
      return identity.role == UserRole::NORMAL || identity.role == UserRole::VIP || identity.role == UserRole::ADMIN;
    case Capability::USE_VIP_BENEFITS:
      return identity.role == UserRole::VIP && identity.vip_status == VipStatus::ACTIVE;
    case Capability::MANAGE_USERS:
    case Capability::DELETE_ANY_FILE:
      return identity.role == UserRole::ADMIN;
  }
  return false;
}

std::size_t role_size_limit(UserRole role, const ServerConfig& config) noexcept;

} // namespace hps
