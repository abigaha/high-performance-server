#pragma once

namespace hps {

class IDatabasePool;
class IAuthService;
class IHttpServer;

void register_auth_routes(IHttpServer& server, IDatabasePool& db, IAuthService& auth);

} // namespace hps
