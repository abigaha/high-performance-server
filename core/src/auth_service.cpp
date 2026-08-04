#include "auth_service.h"

#include "authorization.h"
#include "database_pool.h"
#include "iconnection.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace hps {

namespace {

constexpr std::chrono::seconds kTokenLifetime = std::chrono::hours{24};

std::string to_hex(const unsigned char* data, std::size_t len) {
  static constexpr std::array<char, 17> kHexChars = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f', '\0'};
  std::string hex(len * 2, '\0');
  for (std::size_t i = 0; i < len; ++i) {
    auto idx = i * 2;
    auto high = static_cast<unsigned int>(data[i]) >> 4U;
    auto low = static_cast<unsigned int>(data[i]) & 0x0FU;
    hex[idx] = kHexChars[high];
    hex[idx + 1] = kHexChars[low];
  }
  return hex;
}

std::string hmac_sha256(const std::string& key, const std::string& data) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
  unsigned int hash_len = 0;
  HMAC(EVP_sha256(),
       key.data(),
       static_cast<int>(key.size()),
       reinterpret_cast<const unsigned char*>(data.data()),
       data.size(),
       hash.data(),
       &hash_len);
  return to_hex(hash.data(), hash_len);
}

std::string base64_encode(const std::string& in) {
  static constexpr std::string_view kBase64Chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);
  auto remain = in.size();
  const auto* bytes = reinterpret_cast<const unsigned char*>(in.data());
  while (remain >= 3) {
    auto b0 = static_cast<unsigned int>(bytes[0]);
    auto b1 = static_cast<unsigned int>(bytes[1]);
    auto b2 = static_cast<unsigned int>(bytes[2]);
    out += kBase64Chars[b0 >> 2U];
    out += kBase64Chars[((b0 << 4U) | (b1 >> 4U)) & 0x3FU];
    out += kBase64Chars[((b1 << 2U) | (b2 >> 6U)) & 0x3FU];
    out += kBase64Chars[b2 & 0x3FU];
    bytes += 3;
    remain -= 3;
  }
  if (remain == 2) {
    auto b0 = static_cast<unsigned int>(bytes[0]);
    auto b1 = static_cast<unsigned int>(bytes[1]);
    out += kBase64Chars[b0 >> 2U];
    out += kBase64Chars[((b0 << 4U) | (b1 >> 4U)) & 0x3FU];
    out += kBase64Chars[(b1 << 2U) & 0x3FU];
    out += '=';
  } else if (remain == 1) {
    auto b0 = static_cast<unsigned int>(bytes[0]);
    out += kBase64Chars[b0 >> 2U];
    out += kBase64Chars[(b0 << 4U) & 0x3FU];
    out += "==";
  }
  return out;
}

template <typename Integer>
bool parse_integer(std::string_view value, Integer& result) noexcept {
  if (value.empty()) {
    return false;
  }
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
  return error == std::errc{} && end == value.data() + value.size();
}

std::optional<int64_t> epoch_seconds(std::chrono::system_clock::time_point value) noexcept {
  const auto seconds = std::chrono::floor<std::chrono::seconds>(value).time_since_epoch().count();
  if (!std::in_range<int64_t>(seconds)) {
    return std::nullopt;
  }
  return static_cast<int64_t>(seconds);
}

std::string role_name(UserRole role) {
  switch (role) {
    case UserRole::GUEST:
      return "GUEST";
    case UserRole::NORMAL:
      return "NORMAL";
    case UserRole::VIP:
      return "VIP";
    case UserRole::ADMIN:
      return "ADMIN";
  }
  return "GUEST";
}

std::string vip_status_name(VipStatus status) {
  switch (status) {
    case VipStatus::NONE:
      return "NONE";
    case VipStatus::ACTIVE:
      return "ACTIVE";
    case VipStatus::EXPIRED:
      return "EXPIRED";
  }
  return "NONE";
}

std::string api_datetime(std::string_view mysql_datetime) {
  const auto parsed = parse_mysql_utc_datetime(mysql_datetime);
  if (!parsed) {
    throw std::runtime_error("invalid created_at");
  }
  return format_rfc3339_utc(*parsed);
}

AuthenticatedUserProfile make_authenticated_user_profile(const User& user) {
  return {user.user_id, user.username, user.email, user.created_at};
}

} // namespace

std::string generate_salt() {
  std::array<unsigned char, 16> salt{};
  RAND_bytes(salt.data(), static_cast<int>(salt.size()));
  return to_hex(salt.data(), salt.size());
}

std::string hash_password(const std::string& password, const std::string& salt) {
  auto salted = salt + password;
  std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
  unsigned int hash_len = 0;
  EVP_Digest(salted.data(), salted.size(), hash.data(), &hash_len, EVP_sha256(), nullptr);
  return to_hex(hash.data(), hash_len);
}

class SimpleAuthService : public IAuthService {
public:
  SimpleAuthService(IDatabasePool& db, std::string secret, AuthClock clock) :
      db_(db), secret_(std::move(secret)), clock_(std::move(clock)) {}

  TokenValidationResult validate_token(const std::string& token) override {
    try {
      const auto dot = token.find('.');
      if (dot == std::string::npos || dot == 0 || dot == token.size() - 1) {
        return {};
      }
      auto payload_b64 = token.substr(0, dot);
      const auto sig_hex = token.substr(dot + 1);
      const auto expected_signature = hmac_sha256(secret_, payload_b64);
      if (sig_hex.size() != expected_signature.size() ||
          CRYPTO_memcmp(sig_hex.data(), expected_signature.data(), expected_signature.size()) != 0) {
        return {};
      }

      const auto pad = payload_b64.size() % 4;
      if (pad > 0) {
        payload_b64.append(4 - pad, '=');
      }
      const auto decoded = base64_decode(payload_b64);
      if (!decoded || !decoded->starts_with("uid")) {
        return {};
      }
      const auto role_pos = decoded->find("role", 3);
      const auto exp_pos = role_pos == std::string::npos ? std::string::npos : decoded->find("exp", role_pos + 4);
      if (role_pos == std::string::npos || exp_pos == std::string::npos || role_pos == 3 || exp_pos == role_pos + 4) {
        return {};
      }

      int64_t user_id = 0;
      int64_t expires_at = 0;
      const auto payload = std::string_view(*decoded);
      const auto now = clock_();
      const auto now_seconds = epoch_seconds(now);
      if (!parse_integer(payload.substr(3, role_pos - 3), user_id) || user_id <= 0 ||
          !parse_integer(payload.substr(exp_pos + 3), expires_at) || !now_seconds || *now_seconds >= expires_at) {
        return {};
      }
      const auto user = db_.get_user_result(user_id);
      if (user.status == LookupStatus::NOT_FOUND) {
        return {TokenValidationStatus::USER_NOT_FOUND, {}, {}};
      }
      if (user.status != LookupStatus::FOUND || !user.value) {
        return {TokenValidationStatus::STORAGE_ERROR, {}, {}};
      }
      return {TokenValidationStatus::AUTHENTICATED,
              make_effective_identity(*user.value, now),
              make_authenticated_user_profile(*user.value)};
    } catch (...) {
      return {TokenValidationStatus::STORAGE_ERROR, {}, {}};
    }
  }

  std::string generate_token(const AuthUser& user) override {
    const auto now = clock_();
    const auto lifetime = std::chrono::duration_cast<std::chrono::system_clock::duration>(kTokenLifetime);
    if (now > std::chrono::system_clock::time_point::max() - lifetime) {
      return {};
    }
    const auto expires_at = epoch_seconds(now + lifetime);
    if (!expires_at) {
      return {};
    }
    auto payload = "uid" + std::to_string(user.user_id) + "role" + std::to_string(static_cast<int>(user.role)) + "exp" +
                   std::to_string(*expires_at);
    auto payload_b64 = base64_encode(payload);
    auto sig = hmac_sha256(secret_, payload_b64);
    return payload_b64 + "." + sig;
  }

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  AuthenticationResult authenticate(const std::string& username, const std::string& password) override {
    try {
      const auto auth_user = db_.get_auth_user_result(username);
      if (auth_user.status == LookupStatus::NOT_FOUND) {
        return {AuthenticationStatus::INVALID_CREDENTIALS, std::nullopt};
      }
      if (auth_user.status != LookupStatus::FOUND || !auth_user.value) {
        return {AuthenticationStatus::STORAGE_ERROR, std::nullopt};
      }
      const auto user = db_.get_user_result(auth_user.value->user_id);
      if (user.status == LookupStatus::NOT_FOUND) {
        return {AuthenticationStatus::INVALID_CREDENTIALS, std::nullopt};
      }
      if (user.status != LookupStatus::FOUND || !user.value) {
        return {AuthenticationStatus::STORAGE_ERROR, std::nullopt};
      }
      const auto hashed = hash_password(password, user.value->salt);
      if (hashed != user.value->password_hash) {
        return {AuthenticationStatus::INVALID_CREDENTIALS, std::nullopt};
      }
      return {AuthenticationStatus::AUTHENTICATED, auth_user.value};
    } catch (...) {
      return {AuthenticationStatus::STORAGE_ERROR, std::nullopt};
    }
  }

private:
  static std::optional<std::string> base64_decode(const std::string& in) {
    static constexpr std::string_view kChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int, 256> rev{};
    rev.fill(-1);
    for (int i = 0; i < 64; ++i) {
      rev[static_cast<unsigned char>(kChars[i])] = i;
    }

    if (in.size() % 4 != 0) {
      return std::nullopt;
    }

    std::string out;
    out.reserve(in.size() / 4 * 3);
    for (std::size_t i = 0; i < in.size(); i += 4) {
      auto c0 = rev[static_cast<unsigned char>(in[i])];
      auto c1 = rev[static_cast<unsigned char>(in[i + 1])];
      auto c2 = rev[static_cast<unsigned char>(in[i + 2])];
      auto c3 = rev[static_cast<unsigned char>(in[i + 3])];
      if (c0 < 0 || c1 < 0) {
        return std::nullopt;
      }
      auto uc0 = static_cast<unsigned int>(c0);
      auto uc1 = static_cast<unsigned int>(c1);
      out.push_back(static_cast<char>((uc0 << 2U) | (uc1 >> 4U)));
      if (c2 >= 0) {
        auto uc2 = static_cast<unsigned int>(c2);
        out.push_back(static_cast<char>(((uc1 << 4U) & 0xF0U) | (uc2 >> 2U)));
      }
      if (c3 >= 0) {
        auto uc2 = static_cast<unsigned int>(c2);
        auto uc3 = static_cast<unsigned int>(c3);
        out.push_back(static_cast<char>(((uc2 << 6U) & 0xC0U) | uc3));
      }
    }
    return out;
  }

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
  IDatabasePool& db_;
  std::string secret_;
  AuthClock clock_;
};

std::unique_ptr<IAuthService> create_auth_service(IDatabasePool& db, const std::string& secret, AuthClock clock) {
  if (!clock) {
    clock = [] { return std::chrono::system_clock::now(); };
  }
  return std::make_unique<SimpleAuthService>(db, secret, std::move(clock));
}

nlohmann::json serialize_auth_user(const User& user, const EffectiveIdentity& identity) {
  nlohmann::json capabilities = nlohmann::json::array();
  constexpr std::array capability_names = {
    std::pair{Capability::USE_AUTHENTICATED_FEATURES, "USE_AUTHENTICATED_FEATURES"},
    std::pair{Capability::USE_VIP_BENEFITS, "USE_VIP_BENEFITS"},
    std::pair{Capability::MANAGE_USERS, "MANAGE_USERS"},
    std::pair{Capability::DELETE_ANY_FILE, "DELETE_ANY_FILE"},
  };
  for (const auto& [capability, name] : capability_names) {
    if (has_capability(identity, capability)) {
      capabilities.push_back(name);
    }
  }
  return nlohmann::json{{"user_id", user.user_id},
                        {"username", user.username},
                        {"email", user.email},
                        {"role", role_name(identity.role)},
                        {"vip_status", vip_status_name(identity.vip_status)},
                        {"vip_expires_at",
                         identity.vip_expires_at ? nlohmann::json(format_rfc3339_utc(*identity.vip_expires_at))
                                                 : nlohmann::json(nullptr)},
                        {"capabilities", std::move(capabilities)},
                        {"created_at", api_datetime(user.created_at)}};
}

nlohmann::json serialize_auth_response(const std::string& token, const User& user, const EffectiveIdentity& identity) {
  return nlohmann::json{{"token", token}, {"user", serialize_auth_user(user, identity)}};
}

bool has_forbidden_registration_fields(const nlohmann::json& body) {
  if (!body.is_object()) {
    return false;
  }
  for (const auto& [key, value] : body.items()) {
    static_cast<void>(value);
    std::string normalized;
    normalized.reserve(key.size());
    std::ranges::transform(key, std::back_inserter(normalized), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    if (normalized == "role" || normalized == "capabilities" || normalized.find("vip") != std::string::npos ||
        normalized.find("admin") != std::string::npos) {
      return true;
    }
  }
  return false;
}

} // namespace hps
