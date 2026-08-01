#pragma once

#include "models.h"

#include <chrono>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace hps {

class IDatabasePool;

class IAuthService {
public:
  virtual ~IAuthService() = default;

  virtual TokenValidationResult validate_token(const std::string& token) = 0;
  virtual std::string generate_token(const AuthUser& user) = 0;
  virtual AuthenticationResult authenticate(const std::string& username, const std::string& password) = 0;
};

using AuthClock = std::function<std::chrono::system_clock::time_point()>;

std::unique_ptr<IAuthService> create_auth_service(IDatabasePool& db, const std::string& secret, AuthClock clock = {});
nlohmann::json serialize_auth_user(const User& user, const EffectiveIdentity& identity);
nlohmann::json serialize_auth_response(const std::string& token, const User& user, const EffectiveIdentity& identity);
bool has_forbidden_registration_fields(const nlohmann::json& body);

// 加盐哈希工具
std::string generate_salt();
std::string hash_password(const std::string& password, const std::string& salt);

} // namespace hps
