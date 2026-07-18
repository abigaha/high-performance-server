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
      req.auth_user = AuthUser{};
      return;
    }
    const auto& header = it->second;
    constexpr std::string_view kBearer = "Bearer ";
    if (header.size() <= kBearer.size() || header.substr(0, kBearer.size()) != kBearer) {
      req.auth_user = AuthUser{};
      return;
    }
    auto token = header.substr(kBearer.size());
    req.auth_user = auth_service.validate_token(token);
  }
};

} // namespace hps