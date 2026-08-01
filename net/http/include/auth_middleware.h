#pragma once

#include "auth_service.h"
#include "http_request.h"

#include <memory>
#include <string_view>

namespace hps {

class AuthMiddleware {
public:
  static void apply(IAuthService& auth_service, HttpRequest& req) {
    auto it = req.headers.find("Authorization");
    if (it == req.headers.end()) {
      req.auth_status = TokenValidationStatus::INVALID;
      req.auth_user = EffectiveIdentity{};
      return;
    }
    const auto& header = it->second;
    constexpr std::string_view kBearer = "Bearer ";
    if (header.size() <= kBearer.size() || header.substr(0, kBearer.size()) != kBearer) {
      req.auth_status = TokenValidationStatus::INVALID;
      req.auth_user = EffectiveIdentity{};
      return;
    }
    auto token = header.substr(kBearer.size());
    try {
      auto result = auth_service.validate_token(token);
      req.auth_status = result.status;
      req.auth_user = std::move(result.identity);
    } catch (...) {
      req.auth_status = TokenValidationStatus::STORAGE_ERROR;
      req.auth_user = EffectiveIdentity{};
    }
  }
};

} // namespace hps
