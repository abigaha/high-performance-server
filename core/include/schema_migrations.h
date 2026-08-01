#pragma once

#include "idatabase_pool.h"
#include "models.h"

#include <chrono>
#include <variant>

namespace hps {

MutationResult<std::monostate> run_schema_migrations(IDatabasePool& database,
                                                     std::chrono::system_clock::time_point now);

} // namespace hps
