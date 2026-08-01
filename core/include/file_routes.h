#pragma once

namespace hps {

class IDatabasePool;
class IFileSystem;
class IHttpServer;
class ChunkLifecycleCoordinator;

void register_file_routes(IHttpServer& server,
                          IDatabasePool& database,
                          IFileSystem& file_system,
                          ChunkLifecycleCoordinator& coordinator);

} // namespace hps
