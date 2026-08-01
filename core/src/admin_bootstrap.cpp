#include "admin_bootstrap.h"

#include "auth_service.h"
#include "email_validation.h"
#include "idatabase_pool.h"

#include <string_view>

namespace hps {
namespace {

MutationResult<std::monostate> result(MutationStatus status, std::string_view detail = {}) {
  MutationResult<std::monostate> value;
  value.status = status;
  if (status == MutationStatus::OK) {
    value.value = std::monostate{};
  }
  if (!detail.empty()) {
    value.detail = detail;
  }
  return value;
}

std::optional<MutationResult<std::monostate>> lookup_failure(const LookupResult<User>& lookup,
                                                             std::string_view detail) {
  if (lookup.status == LookupStatus::STORAGE_ERROR || lookup.status == LookupStatus::INVALID_DATA) {
    return result(MutationStatus::STORAGE_ERROR, detail);
  }
  return std::nullopt;
}

MutationResult<std::monostate> validate_config(const AdminBootstrapConfig& config) {
  if (!config.username || !config.password || !config.email || config.username->empty() || config.password->empty() ||
      config.email->empty()) {
    return result(MutationStatus::INVALID_STATE, "ADMIN_CONFIG_INCOMPLETE");
  }
  if (config.username->size() < 2 || config.username->size() > 64) {
    return result(MutationStatus::INVALID_STATE, "ADMIN_USERNAME_INVALID");
  }
  if (config.password->size() < 16) {
    return result(MutationStatus::INVALID_STATE, "ADMIN_PASSWORD_INVALID");
  }
  if (!is_valid_email(*config.email)) {
    return result(MutationStatus::INVALID_STATE, "ADMIN_EMAIL_INVALID");
  }
  return result(MutationStatus::OK);
}

MutationResult<std::monostate> update_existing_admin(IDatabasePool& database,
                                                     const User& admin,
                                                     const AdminBootstrapConfig& config) {
  if (admin.username != *config.username) {
    return result(MutationStatus::CONFLICT, "ADMIN_ACCOUNT_CONFLICT");
  }
  if (admin.email != *config.email) {
    const auto email_owner = database.get_user_by_email_result(*config.email);
    if (const auto failure = lookup_failure(email_owner, "ADMIN_EMAIL_LOOKUP_FAILED")) {
      return *failure;
    }
    if (email_owner.status == LookupStatus::FOUND && email_owner.value && email_owner.value->user_id != admin.user_id) {
      return result(MutationStatus::CONFLICT, "ADMIN_EMAIL_CONFLICT");
    }
  }

  const bool password_matches = hash_password(*config.password, admin.salt) == admin.password_hash;
  if (password_matches && admin.email == *config.email && !admin.vip_expires_at) {
    return result(MutationStatus::OK);
  }
  User updated = admin;
  updated.email = *config.email;
  updated.vip_expires_at.reset();
  if (!password_matches) {
    updated.salt = generate_salt();
    updated.password_hash = hash_password(*config.password, updated.salt);
  }
  const auto update = database.update_admin_credentials(updated);
  if (update.status != MutationStatus::OK) {
    return result(update.status, update.detail.value_or("ADMIN_UPDATE_FAILED"));
  }
  return result(MutationStatus::OK);
}

} // namespace

MutationResult<std::monostate> bootstrap_admin(IDatabasePool& database, const AdminBootstrapConfig& config) {
  const bool all_unset = !config.username && !config.password && !config.email;
  const bool all_empty = config.username && config.password && config.email && config.username->empty() &&
                         config.password->empty() && config.email->empty();
  if (all_unset || all_empty) {
    return result(MutationStatus::OK);
  }
  auto validation = validate_config(config);
  if (validation.status != MutationStatus::OK) {
    return validation;
  }

  const auto admin = database.get_admin_user_result();
  if (const auto failure = lookup_failure(admin, "ADMIN_LOOKUP_FAILED")) {
    return *failure;
  }
  if (admin.status == LookupStatus::FOUND && admin.value) {
    return update_existing_admin(database, *admin.value, config);
  }

  const auto username_owner = database.get_user_by_username_result(*config.username);
  if (const auto failure = lookup_failure(username_owner, "ADMIN_USERNAME_LOOKUP_FAILED")) {
    return *failure;
  }
  if (username_owner.status == LookupStatus::FOUND) {
    return result(MutationStatus::CONFLICT, "ADMIN_USERNAME_CONFLICT");
  }
  const auto email_owner = database.get_user_by_email_result(*config.email);
  if (const auto failure = lookup_failure(email_owner, "ADMIN_EMAIL_LOOKUP_FAILED")) {
    return *failure;
  }
  if (email_owner.status == LookupStatus::FOUND) {
    return result(MutationStatus::CONFLICT, "ADMIN_EMAIL_CONFLICT");
  }

  User user;
  user.username = *config.username;
  user.email = *config.email;
  user.role = UserRole::ADMIN;
  user.vip_expires_at.reset();
  user.salt = generate_salt();
  user.password_hash = hash_password(*config.password, user.salt);
  const auto create = database.create_admin_user(user);
  if (create.status != MutationStatus::OK) {
    return result(create.status, create.detail.value_or("ADMIN_CREATE_FAILED"));
  }
  return result(MutationStatus::OK);
}

} // namespace hps
