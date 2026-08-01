#pragma once

#include "models.h"

#include <optional>
#include <string>
#include <variant>

namespace hps {

class IDatabasePool;

struct AdminBootstrapConfig {
  std::optional<std::string> username;
  std::optional<std::string> password;
  std::optional<std::string> email;
};

MutationResult<std::monostate> bootstrap_admin(IDatabasePool& database, const AdminBootstrapConfig& config);

} // namespace hps
