#include "auth_service.h"

#include "database_pool.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <vector>

namespace hps {

namespace {

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
  explicit SimpleAuthService(IDatabasePool& db, std::string secret) : db_(db), secret_(std::move(secret)) {}

  AuthUser validate_token(const std::string& token) override {
    auto dot = token.find('.');
    if (dot == std::string::npos || dot == 0 || dot == token.size() - 1) {
      return AuthUser{};
    }
    auto payload_b64 = token.substr(0, dot);
    auto sig_hex = token.substr(dot + 1);

    auto expected_sig = hmac_sha256(secret_, payload_b64);
    if (sig_hex != expected_sig) {
      return AuthUser{};
    }

    std::string payload;
    auto pad = payload_b64.size() % 4;
    if (pad > 0) {
      payload_b64.append(4 - pad, '=');
    }
    auto decoded = base64_decode(payload_b64);
    if (!decoded) {
      return AuthUser{};
    }
    payload = std::move(*decoded);
    auto uid_pos = payload.find("uid");
    auto role_pos = payload.find("role");
    auto exp_pos = payload.find("exp");
    if (uid_pos == std::string::npos || role_pos == std::string::npos || exp_pos == std::string::npos) {
      return AuthUser{};
    }

    auto uid = std::stoll(payload.substr(uid_pos + 3));
    auto exp = std::stoll(payload.substr(exp_pos + 3));
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    if (now > exp) {
      return AuthUser{};
    }

    int role_val = std::stoi(payload.substr(role_pos + 4));
    UserRole role = UserRole::GUEST;
    if (role_val >= 2) {
      role = UserRole::VIP;
    } else if (role_val >= 1) {
      role = UserRole::NORMAL;
    }

    AuthUser u;
    u.user_id = uid;
    u.role = role;
    u.username = "user_" + std::to_string(uid);
    return u;
  }

  std::string generate_token(const AuthUser& user) override {
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    auto payload = "uid" + std::to_string(user.user_id) + "role" + std::to_string(static_cast<int>(user.role)) + "exp" +
                   std::to_string(now + 86400);
    auto payload_b64 = base64_encode(payload);
    auto sig = hmac_sha256(secret_, payload_b64);
    return payload_b64 + "." + sig;
  }

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  std::optional<AuthUser> authenticate(const std::string& username, const std::string& password) override {
    auto auth_user = db_.get_auth_user(username);
    if (!auth_user) {
      return std::nullopt;
    }
    auto user = db_.get_user(auth_user->user_id);
    if (!user) {
      return std::nullopt;
    }
    auto hashed = hash_password(password, user->salt);
    if (hashed != user->password_hash) {
      return std::nullopt;
    }
    return auth_user;
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
};

std::unique_ptr<IAuthService> create_auth_service(IDatabasePool& db, const std::string& secret) {
  return std::make_unique<SimpleAuthService>(db, secret);
}

} // namespace hps