#pragma once

#include "models.h"

#include <memory>
#include <optional>
#include <string>

namespace hps {

class IDatabasePool;

class IAuthService {
public:
  virtual ~IAuthService() = default;

  virtual AuthUser validate_token(const std::string& token) = 0;
  virtual std::string generate_token(const AuthUser& user) = 0;
  virtual std::optional<AuthUser> authenticate(const std::string& username, const std::string& password) = 0;
};

std::unique_ptr<IAuthService> create_auth_service(IDatabasePool& db, const std::string& secret);

} // namespace hps