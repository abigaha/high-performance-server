#pragma once

#include "http_server.h"

namespace hps {

class IDatabasePool;
class IFileSystem;
class ChunkLifecycleCoordinator;

HttpServer::UploadStreamSetup make_upload_setup(const IDatabasePool& database,
                                                IFileSystem& file_system,
                                                ChunkLifecycleCoordinator& coordinator);

} // namespace hps
