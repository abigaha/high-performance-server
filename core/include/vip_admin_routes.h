#pragma once

#include "i_http_server.h"

#include <chrono>
#include <functional>

namespace hps {

class IDatabasePool;

using VipAdminClock = std::function<std::chrono::system_clock::time_point()>;

void register_vip_routes(IHttpServer& server, IDatabasePool& database, VipAdminClock clock);
void register_admin_routes(IHttpServer& server, IDatabasePool& database, VipAdminClock clock);

} // namespace hps
