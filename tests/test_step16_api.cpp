#include "auth_middleware.h"
#include "auth_routes.h"
#include "auth_service.h"
#include "authorization.h"
#include "database_pool.h"
#include "db_config.h"
#include "file_routes.h"
#include "http_parser.h"
#include "http_request.h"
#include "http_response.h"
#include "i_file_system.h"
#include "i_http_server.h"
#include "logappender.h"
#include "logger.h"
#include "mock_connection.h"
#include "models.h"
#include "pending_chunk_deletions.h"
#include "playlist_routes.h"
#include "range_parser.h"
#include "strict_json.h"
#include "upload_setup.h"
#include "vip_admin_routes.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hps {

namespace {

class CapturingHttpServer final : public IHttpServer {
public:
  bool init() override { return true; }

  void start() override {}

  void stop() override {}

  void get(std::string_view path, Handler handler) override { get_handlers[std::string(path)] = std::move(handler); }

  void post(std::string_view path, Handler handler) override { post_handlers[std::string(path)] = std::move(handler); }

  void put(std::string_view path, Handler handler) override { put_handlers[std::string(path)] = std::move(handler); }

  void del(std::string_view path, Handler handler) override { del_handlers[std::string(path)] = std::move(handler); }

  void ws(std::string_view path, WsHandler handler) override {
    (void)path;
    (void)handler;
  }

  uint16_t actual_port() const override { return 0; }

  std::unordered_map<std::string, Handler> get_handlers;
  std::unordered_map<std::string, Handler> post_handlers;
  std::unordered_map<std::string, Handler> put_handlers;
  std::unordered_map<std::string, Handler> del_handlers;
};

class RouteFileSystem final : public IFileSystem {
public:
  ChunkDeleteStatus delete_status{ChunkDeleteStatus::NOT_FOUND};
  std::function<void()> store_hook;

  std::vector<FileChunk> split_file(const std::string& path, std::size_t chunk_size) override {
    (void)path;
    (void)chunk_size;
    return {};
  }

  std::string compute_file_hash(const std::string& path) override {
    (void)path;
    return {};
  }

  std::string compute_chunk_hash(const FileChunk& chunk) override {
    (void)chunk;
    return {};
  }

  bool store_file(const std::string& path, const std::vector<char>& data) override {
    (void)path;
    (void)data;
    if (store_hook) {
      store_hook();
    }
    return true;
  }

  bool delete_file(const std::string& path) override {
    (void)path;
    return false;
  }

  ChunkDeleteStatus delete_file_status(const std::string& path) override {
    (void)path;
    return delete_status;
  }

  std::optional<std::vector<char>> read_file(const std::string& path) override {
    (void)path;
    return std::nullopt;
  }
};

class CapturingLogAppender final : public LogAppender {
public:
  void log(LogLevel level, const LogEvent::ptr event) override {
    (void)level;
    messages.push_back(event->getContent());
  }

  std::vector<std::string> messages;
};

std::unique_ptr<DatabasePool> make_database_pool(MockConnection*& connection) {
  auto pool = std::make_unique<DatabasePool>([&connection]() -> std::unique_ptr<IConnection> {
    auto mock = std::make_unique<MockConnection>();
    connection = mock.get();
    return mock;
  });
  DbConfig config;
  config.pool_size = 1;
  if (!pool->init(config)) {
    throw std::runtime_error("failed to initialize test database pool");
  }
  return pool;
}

std::string mysql_datetime_text(const std::string& value) {
  return value;
}

std::string mysql_datetime_text(const std::optional<std::string>& value) {
  return value.value_or("");
}

class EmptyTokenAuthService final : public IAuthService {
public:
  explicit EmptyTokenAuthService(AuthUser user) : user_(std::move(user)) {}

  TokenValidationResult validate_token(const std::string& token) override {
    (void)token;
    return {};
  }

  std::string generate_token(const AuthUser& user) override {
    (void)user;
    return {};
  }

  AuthenticationResult authenticate(const std::string& username, const std::string& password) override {
    (void)username;
    (void)password;
    return {AuthenticationStatus::AUTHENTICATED, user_};
  }

private:
  AuthUser user_;
};

constexpr auto kRouteNow = std::chrono::system_clock::time_point{std::chrono::seconds{2'000'000'000}};

EffectiveIdentity identity(int64_t user_id, UserRole role) {
  EffectiveIdentity result;
  result.user_id = user_id;
  result.username = "actor";
  result.role = role;
  result.vip_status = role == UserRole::VIP ? VipStatus::ACTIVE : VipStatus::NONE;
  return result;
}

HttpRequest authenticated_request(int64_t user_id, UserRole role) {
  HttpRequest request;
  request.auth_status = TokenValidationStatus::AUTHENTICATED;
  request.auth_user = identity(user_id, role);
  return request;
}

void wait_for_cleanup_waiter(const ChunkLifecycleCoordinator& coordinator) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (coordinator.cleanup_waiters() == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  ASSERT_GT(coordinator.cleanup_waiters(), 0U);
}

} // namespace

TEST(StrictJsonTest, IntegerTypeAcceptsSignedAndBoundedUnsignedValuesOnly) {
  const nlohmann::json signed_value = int64_t{-7};
  const nlohmann::json unsigned_value = uint64_t{7};
  const nlohmann::json maximum_value = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  const nlohmann::json oversized_value = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1U;
  const nlohmann::json floating_value = 7.0;

  EXPECT_TRUE(matches_strict_json_type(signed_value, StrictJsonValueType::INTEGER));
  EXPECT_TRUE(matches_strict_json_type(unsigned_value, StrictJsonValueType::INTEGER));
  EXPECT_TRUE(matches_strict_json_type(unsigned_value, StrictJsonFieldType::INTEGER));
  EXPECT_TRUE(matches_strict_json_type(maximum_value, StrictJsonValueType::INTEGER));
  EXPECT_FALSE(matches_strict_json_type(oversized_value, StrictJsonValueType::INTEGER));
  EXPECT_FALSE(matches_strict_json_type(floating_value, StrictJsonValueType::INTEGER));

  EXPECT_FALSE(
    parse_strict_json_object(R"({"value":-1})", {{"value", StrictJsonValueType::INTEGER, true, true}}).has_value());
}

TEST(Step19FileRouteTest, ListStrictlyDecodesQueryAndReturnsDeletionCapabilities) {
  MockConnection* connection = nullptr;
  auto database = make_database_pool(connection);
  std::vector<std::string> transaction_statements;
  connection->execute_hook = [&transaction_statements](const std::string& sql, const std::vector<std::string>&) {
    transaction_statements.push_back(sql);
    return std::optional<int64_t>{0};
  };
  connection->query_hook = [](const std::string& sql, const std::vector<std::string>& params) {
    EXPECT_NE(sql.find("COUNT(*) OVER() AS total"), std::string::npos);
    EXPECT_EQ(params, (std::vector<std::string>{"%报告%", "audio", "20", "0"}));
    return std::optional<QueryResult>{
      QueryResult{.rows = {
                    {"7", "mine.mp3", "hash-1", "10", "audio/mpeg", "4", "2026-01-01 00:00:00.000000", "0", "42", "2"},
                    {"8", "other.mp3", "hash-2", "20", "audio/mpeg", "4", "2026-01-02 00:00:00.000000", "0", "99", "2"},
                  }}};
  };
  CapturingHttpServer server;
  RouteFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  ASSERT_TRUE(database->bind_chunk_lifecycle_coordinator(coordinator));
  register_file_routes(server, *database, file_system, coordinator);

  auto request = authenticated_request(42, UserRole::VIP);
  request.query_string = "name=%E6%8A%A5%E5%91%8A&type=audio&offset=0&limit=20";
  HttpResponse response;
  server.get_handlers.at("/api/files")(request, response);

  ASSERT_EQ(response.status_code, 200);
  const auto body = nlohmann::json::parse(response.body);
  EXPECT_TRUE(body["items"][0]["can_delete"].get<bool>());
  EXPECT_FALSE(body["items"][1]["can_delete"].get<bool>());
  EXPECT_EQ(body["items"][0]["uploaded_by"], 42);
  EXPECT_EQ(body["items"][0]["created_at"], "2026-01-01T00:00:00.000000Z");
  EXPECT_TRUE(transaction_statements.empty());

  request.query_string = "type=audio&type=video";
  server.get_handlers.at("/api/files")(request, response);
  EXPECT_EQ(response.status_code, 400);
  EXPECT_EQ(nlohmann::json::parse(response.body)["code"], "INVALID_REQUEST");

  request.query_string = "type=audio&";
  server.get_handlers.at("/api/files")(request, response);
  EXPECT_EQ(response.status_code, 400);
}

TEST(Step19FileRouteTest, DetailRequiresAuthenticationAndReturnsCompleteDeletionContract) {
  MockConnection* connection = nullptr;
  auto database = make_database_pool(connection);
  connection->query_result = QueryResult{
    .rows = {
      {"7", "detail.mp3", "hash-7", "1024", "audio/mpeg", "2097152", "2026-01-03 00:00:00.000000", "0", "42"},
    }};
  CapturingHttpServer server;
  RouteFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  register_file_routes(server, *database, file_system, coordinator);

  ASSERT_TRUE(server.get_handlers.contains("/api/files/:id"));
  const auto invoke = [&](HttpRequest request) {
    request.path_params["id"] = "7";
    HttpResponse response;
    server.get_handlers.at("/api/files/:id")(request, response);
    return response;
  };

  const auto unauthenticated = invoke(HttpRequest{});
  EXPECT_EQ(unauthenticated.status_code, 401);
  EXPECT_EQ(nlohmann::json::parse(unauthenticated.body).at("code"), "AUTH_REQUIRED");

  const auto owner = invoke(authenticated_request(42, UserRole::NORMAL));
  ASSERT_EQ(owner.status_code, 200);
  EXPECT_EQ(nlohmann::json::parse(owner.body),
            (nlohmann::json{{"file_id", 7},
                            {"file_name", "detail.mp3"},
                            {"file_hash", "hash-7"},
                            {"file_size", 1024},
                            {"content_type", "audio/mpeg"},
                            {"uploaded_by", 42},
                            {"can_delete", true},
                            {"created_at", "2026-01-03T00:00:00.000000Z"}}));

  EXPECT_FALSE(
    nlohmann::json::parse(invoke(authenticated_request(99, UserRole::VIP)).body).at("can_delete").get<bool>());
  EXPECT_TRUE(
    nlohmann::json::parse(invoke(authenticated_request(1, UserRole::ADMIN)).body).at("can_delete").get<bool>());
}

TEST(Step19FileRouteTest, DetailMapsStructuredLookupFailuresToStableResponses) {
  const auto invoke = [](const auto& configure, int expected_status, const std::string& expected_code) {
    MockConnection* connection = nullptr;
    auto database = make_database_pool(connection);
    ASSERT_NE(connection, nullptr);
    configure(*connection);
    CapturingHttpServer server;
    RouteFileSystem file_system;
    ChunkLifecycleCoordinator coordinator;
    register_file_routes(server, *database, file_system, coordinator);
    auto request = authenticated_request(42, UserRole::NORMAL);
    request.path_params["id"] = "7";
    HttpResponse response;

    server.get_handlers.at("/api/files/:id")(request, response);

    EXPECT_EQ(response.status_code, expected_status);
    EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), expected_code);
  };

  invoke([](MockConnection& connection) { connection.query_result = QueryResult{}; }, 404, "FILE_NOT_FOUND");
  invoke([](MockConnection& connection) { connection.query_result = std::nullopt; }, 500, "PERSISTENCE_ERROR");
  invoke([](MockConnection& connection) { connection.query_result = QueryResult{.rows = {{"truncated"}}}; },
         422,
         "INVALID_STATE");
  invoke(
    [](MockConnection& connection) {
      connection.query_hook = [](const std::string&, const std::vector<std::string>&) -> std::optional<QueryResult> {
        throw std::runtime_error("storage failure");
      };
    },
    500,
    "PERSISTENCE_ERROR");
}

TEST(Step19FileRouteTest, DeleteAllowsOwnerAndAdminButRejectsOtherVip) {
  const auto invoke = [](UserRole role, int64_t actor, int64_t owner) {
    MockConnection* connection = nullptr;
    auto database = make_database_pool(connection);
    connection->execute_hook = [](const std::string&, const std::vector<std::string>&) {
      return std::optional<int64_t>{1};
    };
    connection->query_hook = [owner](const std::string& sql, const std::vector<std::string>&) {
      if (sql == "SELECT music_id FROM file_records WHERE file_id = ?") {
        return std::optional<QueryResult>{QueryResult{.rows = {{""}}}};
      }
      if (sql.find("WHERE file_id = ?") != std::string::npos) {
        return std::optional<QueryResult>{QueryResult{.rows = {{"7",
                                                                "file.bin",
                                                                "hash",
                                                                "10",
                                                                "application/octet-stream",
                                                                "4",
                                                                "2026-01-01",
                                                                "0",
                                                                std::to_string(owner)}}}};
      }
      return std::optional<QueryResult>{QueryResult{}};
    };
    CapturingHttpServer server;
    RouteFileSystem file_system;
    ChunkLifecycleCoordinator coordinator;
    EXPECT_TRUE(database->bind_chunk_lifecycle_coordinator(coordinator));
    register_file_routes(server, *database, file_system, coordinator);
    auto request = authenticated_request(actor, role);
    request.path_params["id"] = "7";
    HttpResponse response;
    server.del_handlers.at("/api/files/:id")(request, response);
    return response;
  };

  EXPECT_EQ(invoke(UserRole::NORMAL, 42, 42).status_code, 200);
  const auto forbidden = invoke(UserRole::VIP, 99, 42);
  EXPECT_EQ(forbidden.status_code, 403);
  EXPECT_EQ(nlohmann::json::parse(forbidden.body)["code"], "FILE_DELETE_FORBIDDEN");
  EXPECT_EQ(invoke(UserRole::ADMIN, 1, 42).status_code, 200);
}

TEST(Step19FileRouteTest, DeleteKeepsCommittedSuccessAndMarksDeferredCleanupOnConsumerFailure) {
  MockConnection* connection = nullptr;
  auto database = make_database_pool(connection);
  connection->execute_hook = [](const std::string& sql, const std::vector<std::string>&) {
    if (sql.find("pending_chunk_deletions") != std::string::npos)
      return std::optional<int64_t>{};
    return std::optional<int64_t>{1};
  };
  connection->query_hook = [](const std::string& sql, const std::vector<std::string>&) {
    if (sql == "SELECT music_id FROM file_records WHERE file_id = ?")
      return std::optional<QueryResult>{QueryResult{.rows = {{""}}}};
    if (sql.find("WHERE file_id = ? FOR UPDATE") != std::string::npos)
      return std::optional<QueryResult>{QueryResult{
        .rows = {{"7", "file.bin", "hash", "10", "application/octet-stream", "4", "2026-01-01", "", "42"}}}};
    return std::optional<QueryResult>{QueryResult{}};
  };
  CapturingHttpServer server;
  RouteFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  ASSERT_TRUE(database->bind_chunk_lifecycle_coordinator(coordinator));
  register_file_routes(server, *database, file_system, coordinator);
  auto request = authenticated_request(42, UserRole::NORMAL);
  request.path_params["id"] = "7";
  HttpResponse response;
  auto appender = std::make_shared<CapturingLogAppender>();
  Logger::init("route-test");
  Logger::getInstance().addAppender(appender);

  server.del_handlers.at("/api/files/:id")(request, response);

  ASSERT_EQ(response.status_code, 200);
  EXPECT_TRUE(nlohmann::json::parse(response.body)["cleanup_deferred"].get<bool>());
  ASSERT_FALSE(appender->messages.empty());
  EXPECT_EQ(appender->messages.back(), "pending chunk cleanup deferred after committed file deletion");
  Logger::getInstance().delAppender(appender);
}

TEST(Step19FileRouteTest, DeleteReportsCleanupNotDeferredWhenConsumerSucceeds) {
  MockConnection* connection = nullptr;
  auto database = make_database_pool(connection);
  connection->execute_hook = [](const std::string&, const std::vector<std::string>&) {
    return std::optional<int64_t>{1};
  };
  connection->query_hook = [](const std::string& sql, const std::vector<std::string>&) {
    if (sql == "SELECT music_id FROM file_records WHERE file_id = ?")
      return std::optional<QueryResult>{QueryResult{.rows = {{""}}}};
    if (sql.find("WHERE file_id = ? FOR UPDATE") != std::string::npos)
      return std::optional<QueryResult>{QueryResult{
        .rows = {{"7", "file.bin", "hash", "10", "application/octet-stream", "4", "2026-01-01", "", "42"}}}};
    return std::optional<QueryResult>{QueryResult{}};
  };
  CapturingHttpServer server;
  RouteFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  ASSERT_TRUE(database->bind_chunk_lifecycle_coordinator(coordinator));
  register_file_routes(server, *database, file_system, coordinator);
  auto request = authenticated_request(42, UserRole::NORMAL);
  request.path_params["id"] = "7";
  HttpResponse response;

  server.del_handlers.at("/api/files/:id")(request, response);

  ASSERT_EQ(response.status_code, 200);
  EXPECT_FALSE(nlohmann::json::parse(response.body)["cleanup_deferred"].get<bool>());
}

TEST(Step19FileRouteTest, DeleteWaitsForUploadBeforeDatabaseDeleteAndPendingConsumption) {
  MockConnection* connection = nullptr;
  auto database = make_database_pool(connection);
  std::atomic<bool> database_delete_called{false};
  std::atomic<bool> pending_claim_called{false};
  connection->execute_hook = [](const std::string&, const std::vector<std::string>&) {
    return std::optional<int64_t>{1};
  };
  connection->query_hook = [&](const std::string& sql, const std::vector<std::string>&) {
    if (sql == "SELECT music_id FROM file_records WHERE file_id = ?") {
      database_delete_called.store(true);
      return std::optional<QueryResult>{QueryResult{.rows = {{""}}}};
    }
    if (sql.find("FROM file_records WHERE file_id = ? FOR UPDATE") != std::string::npos) {
      return std::optional<QueryResult>{QueryResult{
        .rows = {{"7", "file.bin", "hash", "10", "application/octet-stream", "4", "2026-01-01", "", "42"}}}};
    }
    if (sql.find("FROM pending_chunk_deletions WHERE state = 'PENDING'") != std::string::npos) {
      pending_claim_called.store(true);
    }
    return std::optional<QueryResult>{QueryResult{}};
  };
  CapturingHttpServer server;
  RouteFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  ASSERT_TRUE(database->bind_chunk_lifecycle_coordinator(coordinator));
  register_file_routes(server, *database, file_system, coordinator);
  auto request = authenticated_request(42, UserRole::NORMAL);
  request.path_params["id"] = "7";
  HttpResponse response;
  auto upload_setup = make_upload_setup(*database, file_system, coordinator);
  UploadStreamContext upload_context;
  upload_context.file_name = "track.mp3";
  HttpParser parser;
  upload_setup(request, upload_context, parser);

  auto deletion = std::async(std::launch::async, [&] { server.del_handlers.at("/api/files/:id")(request, response); });

  wait_for_cleanup_waiter(coordinator);
  EXPECT_FALSE(database_delete_called.load());
  EXPECT_FALSE(pending_claim_called.load());
  upload_context.chunk_lifecycle_guard.reset();
  deletion.get();

  EXPECT_TRUE(database_delete_called.load());
  EXPECT_TRUE(pending_claim_called.load());
  EXPECT_EQ(response.status_code, 200);
}

TEST(Step19FileRouteTest, DeleteCleanupBlocksProductionUploadSetupAndPhysicalStore) {
  MockConnection* connection = nullptr;
  auto database = make_database_pool(connection);
  std::mutex mutex;
  std::condition_variable condition;
  bool transaction_started = false;
  bool continue_transaction = false;
  std::atomic<bool> setup_started{false};
  std::atomic<bool> setup_finished{false};
  std::atomic<bool> store_called{false};
  connection->execute_hook = [&](const std::string& sql, const std::vector<std::string>&) {
    if (sql == "START TRANSACTION") {
      std::unique_lock lock{mutex};
      transaction_started = true;
      condition.notify_all();
      condition.wait(lock, [&] { return continue_transaction; });
    }
    return std::optional<int64_t>{1};
  };
  connection->query_hook = [](const std::string& sql, const std::vector<std::string>&) {
    if (sql == "SELECT music_id FROM file_records WHERE file_id = ?")
      return std::optional<QueryResult>{QueryResult{.rows = {{""}}}};
    if (sql.find("FROM file_records WHERE file_id = ? FOR UPDATE") != std::string::npos)
      return std::optional<QueryResult>{QueryResult{
        .rows = {{"7", "file.bin", "hash", "10", "application/octet-stream", "4", "2026-01-01", "", "42"}}}};
    return std::optional<QueryResult>{QueryResult{}};
  };
  CapturingHttpServer server;
  RouteFileSystem file_system;
  file_system.store_hook = [&] { store_called.store(true); };
  ChunkLifecycleCoordinator coordinator;
  ASSERT_TRUE(database->bind_chunk_lifecycle_coordinator(coordinator));
  register_file_routes(server, *database, file_system, coordinator);
  auto upload_setup = make_upload_setup(*database, file_system, coordinator);
  auto request = authenticated_request(42, UserRole::NORMAL);
  request.path_params["id"] = "7";
  HttpResponse response;
  auto deletion = std::async(std::launch::async, [&] { server.del_handlers.at("/api/files/:id")(request, response); });
  {
    std::unique_lock lock{mutex};
    condition.wait(lock, [&] { return transaction_started; });
  }

  UploadStreamContext upload_context;
  upload_context.file_name = "track.mp3";
  HttpParser parser;
  auto upload = std::async(std::launch::async, [&] {
    setup_started.store(true);
    setup_started.notify_one();
    upload_setup(request, upload_context, parser);
    setup_finished.store(true);
    ASSERT_TRUE(upload_context.store_chunk_data("data", "chunk"));
  });
  setup_started.wait(false);
  EXPECT_FALSE(setup_finished.load());
  EXPECT_FALSE(store_called.load());
  {
    std::lock_guard lock{mutex};
    continue_transaction = true;
  }
  condition.notify_all();
  deletion.get();
  upload.get();

  EXPECT_TRUE(setup_finished.load());
  EXPECT_TRUE(store_called.load());
  EXPECT_EQ(response.status_code, 200);
}

TEST(Step19FileRouteTest, FileMusicChangeConflictMapsTo409) {
  MockConnection* connection = nullptr;
  auto database = make_database_pool(connection);
  connection->execute_hook = [](const std::string&, const std::vector<std::string>&) {
    return std::optional<int64_t>{1};
  };
  connection->query_hook = [](const std::string& sql, const std::vector<std::string>&) {
    if (sql == "SELECT music_id FROM file_records WHERE file_id = ?") {
      return std::optional<QueryResult>{QueryResult{.rows = {{"9"}}}};
    }
    if (sql == "SELECT music_id FROM music_meta WHERE music_id = ? FOR UPDATE") {
      return std::optional<QueryResult>{QueryResult{.rows = {{"9"}}}};
    }
    if (sql.find("FROM file_records WHERE file_id = ? FOR UPDATE") != std::string::npos) {
      return std::optional<QueryResult>{QueryResult{
        .rows = {{"7", "file.bin", "hash", "10", "application/octet-stream", "4", "2026-01-01", "10", "42"}}}};
    }
    return std::optional<QueryResult>{QueryResult{}};
  };
  CapturingHttpServer server;
  RouteFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  ASSERT_TRUE(database->bind_chunk_lifecycle_coordinator(coordinator));
  register_file_routes(server, *database, file_system, coordinator);
  auto request = authenticated_request(42, UserRole::NORMAL);
  request.path_params["id"] = "7";
  HttpResponse response;

  server.del_handlers.at("/api/files/:id")(request, response);

  EXPECT_EQ(response.status_code, 409);
  EXPECT_EQ(response.status_text, "Conflict");
  EXPECT_TRUE(response.serialize().starts_with("HTTP/1.1 409 Conflict\r\n"));
  EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "FILE_MUSIC_CHANGED");
}

TEST(Step19FileRouteTest, InvalidDatabaseListUsesStandard422ReasonPhrase) {
  MockConnection* connection = nullptr;
  auto database = make_database_pool(connection);
  connection->execute_hook = [](const std::string&, const std::vector<std::string>&) {
    return std::optional<int64_t>{1};
  };
  connection->query_hook = [](const std::string& sql, const std::vector<std::string>&) {
    if (sql.find("COUNT(*)") != std::string::npos)
      return std::optional<QueryResult>{QueryResult{.rows = {{"1"}}}};
    return std::optional<QueryResult>{QueryResult{.rows = {{"malformed"}}}};
  };
  CapturingHttpServer server;
  RouteFileSystem file_system;
  ChunkLifecycleCoordinator coordinator;
  register_file_routes(server, *database, file_system, coordinator);
  auto request = authenticated_request(42, UserRole::NORMAL);
  HttpResponse response;

  server.get_handlers.at("/api/files")(request, response);

  EXPECT_EQ(response.status_code, 422);
  EXPECT_EQ(response.status_text, "Unprocessable Entity");
}

TEST(Step19StartupOrderTest, MainKeepsMigrationFilesystemCleanupAdminAuthRoutesListenOrder) {
  std::ifstream source("core/src/main.cpp");
  ASSERT_TRUE(source.is_open());
  const std::string text{std::istreambuf_iterator<char>{source}, std::istreambuf_iterator<char>{}};
  const auto migration = text.find("run_schema_migrations");
  const auto filesystem = text.find("make_unique<hps::FileSystem>", migration);
  const auto keep = text.find("chunks/.keep", filesystem);
  const auto cleanup = text.find("run_pending_chunk_deletions", keep);
  const auto admin = text.find("bootstrap_admin", cleanup);
  const auto auth = text.find("create_auth_service", admin);
  const auto routes = text.find("register_routes", auth);
  const auto listen = text.find("server.init()", routes);

  EXPECT_NE(migration, std::string::npos);
  EXPECT_LT(migration, filesystem);
  EXPECT_LT(filesystem, keep);
  EXPECT_LT(keep, cleanup);
  EXPECT_LT(cleanup, admin);
  EXPECT_LT(admin, auth);
  EXPECT_LT(auth, routes);
  EXPECT_LT(routes, listen);
  EXPECT_NE(text.find("startup pending chunk cleanup deferred"), std::string::npos);
}

TEST(Step19PlaylistRouteTest, EveryEndpointRejectsCrossUserIncludingAdmin) {
  MockConnection* connection = nullptr;
  auto database = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  connection->execute_result = 1;
  connection->query_hook = [](const std::string& sql, const std::vector<std::string>&) -> std::optional<QueryResult> {
    if (sql.find("music_meta") != std::string::npos)
      return QueryResult{.rows = {{"3"}}};
    if (sql.find("playlist_items") != std::string::npos)
      return QueryResult{};
    return QueryResult{.rows = {{"42"}}};
  };
  CapturingHttpServer server;
  register_playlist_routes(server, *database);

  for (const auto role : {UserRole::NORMAL, UserRole::ADMIN}) {
    auto request = authenticated_request(99, role);
    request.path_params["id"] = "7";
    request.path_params["music_id"] = "3";
    for (auto* handlers : {&server.get_handlers, &server.post_handlers, &server.put_handlers, &server.del_handlers}) {
      for (const auto& [path, handler] : *handlers) {
        if (!path.starts_with("/api/playlists/"))
          continue;
        if (path == "/api/playlists/:id/items") {
          request.body = R"({"music_id":3})";
        } else if (path == "/api/playlists/:id/items/reorder") {
          request.body = R"({"music_ids":[3]})";
        } else if (path == "/api/playlists/:id") {
          request.body = R"({"name":"renamed","description":"desc"})";
        } else {
          request.body.clear();
        }
        HttpResponse response;
        handler(request, response);
        EXPECT_EQ(response.status_code, 403) << path;
        EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "PLAYLIST_OWNER_REQUIRED") << path;
      }
    }
  }

  auto list = authenticated_request(99, UserRole::ADMIN);
  list.path_params["id"] = "42";
  HttpResponse list_response;
  server.get_handlers.at("/api/users/:id/playlists")(list, list_response);
  EXPECT_EQ(list_response.status_code, 403);
  EXPECT_EQ(nlohmann::json::parse(list_response.body).at("code"), "PLAYLIST_OWNER_REQUIRED");
}

TEST(Step19PlaylistRouteTest, RenameValidatesNameAndDeleteReturnsEmpty204) {
  MockConnection* connection = nullptr;
  auto database = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  connection->execute_result = 1;
  connection->query_hook = [](const std::string& sql, const std::vector<std::string>&) {
    if (sql.find("SELECT p.playlist_id") != std::string::npos)
      return std::optional<QueryResult>{
        QueryResult{.rows = {{"7", "42", "renamed", "desc", "3", "2026-01-02 03:04:05.000000"}}}};
    if (sql.find("user_playlists") != std::string::npos) {
      return std::optional<QueryResult>{QueryResult{.rows = {{"42"}}}};
    }
    return std::optional<QueryResult>{QueryResult{}};
  };
  CapturingHttpServer server;
  register_playlist_routes(server, *database);
  auto request = authenticated_request(42, UserRole::NORMAL);
  request.path_params["id"] = "7";

  for (const std::string& name : {std::string{}, std::string(129, 'x')}) {
    request.body = nlohmann::json{{"name", name}, {"description", "desc"}}.dump();
    HttpResponse invalid;
    server.put_handlers.at("/api/playlists/:id")(request, invalid);
    EXPECT_EQ(invalid.status_code, 400);
    EXPECT_EQ(nlohmann::json::parse(invalid.body).at("code"), "INVALID_REQUEST");
  }

  request.body = R"({"name":"renamed","description":"desc"})";
  HttpResponse renamed;
  server.put_handlers.at("/api/playlists/:id")(request, renamed);
  EXPECT_EQ(renamed.status_code, 200);

  HttpResponse deleted;
  server.del_handlers.at("/api/playlists/:id")(request, deleted);
  EXPECT_EQ(deleted.status_code, 204);
  EXPECT_TRUE(deleted.body.empty());
}

TEST(Step19PlaylistRouteTest, StructuredDatabaseStatusesHaveStableHttpMappings) {
  struct Case {
    MutationStatus status;
    std::optional<std::string> detail;
    int http_status;
    std::string code;
  };

  const std::vector<Case> cases = {
    {MutationStatus::NOT_FOUND, "PLAYLIST_NOT_FOUND", 404, "PLAYLIST_NOT_FOUND"},
    {MutationStatus::NOT_FOUND, "MUSIC_NOT_FOUND", 404, "MUSIC_NOT_FOUND"},
    {MutationStatus::OWNER_REQUIRED, std::nullopt, 403, "PLAYLIST_OWNER_REQUIRED"},
    {MutationStatus::INVALID_STATE, std::nullopt, 400, "INVALID_REQUEST"},
    {MutationStatus::CONFLICT, std::nullopt, 409, "PLAYLIST_ORDER_CONFLICT"},
    {MutationStatus::STORAGE_ERROR, std::nullopt, 500, "PERSISTENCE_ERROR"},
  };
  for (const auto& test_case : cases) {
    class StatusDatabase final : public IDatabasePool {
    public:
      StatusDatabase(MutationStatus value, std::optional<std::string> result_detail) :
          status(value), detail(std::move(result_detail)) {}

      bool init(const DbConfig& config) override {
        (void)config;
        return true;
      }

      void close() override {}

      LookupResult<User> get_user_result(int64_t user_id) override {
        (void)user_id;
        return {};
      }

      MutationResult<std::monostate> create_user(const User& user) override {
        (void)user;
        return {};
      }

      MutationResult<std::monostate> update_user(const User& user) override {
        (void)user;
        return {};
      }

      bool username_exists(const std::string& username) override {
        (void)username;
        return false;
      }

      std::optional<int64_t> store_file_record(const FileRecord& record) override {
        (void)record;
        return {};
      }

      std::optional<FileRecord> get_file_record(int64_t file_id) override {
        (void)file_id;
        return {};
      }

      LookupResult<FileRecord> get_file_record_result(int64_t file_id) override {
        (void)file_id;
        return {LookupStatus::STORAGE_ERROR, std::nullopt};
      }

      std::optional<FileRecord> get_file_record_by_hash(const std::string& hash) override {
        (void)hash;
        return {};
      }

      std::vector<FileRecord> search_files(const std::string& name_pattern, int offset, int limit) override {
        (void)name_pattern;
        (void)offset;
        (void)limit;
        return {};
      }

      std::vector<FileRecord> search_files_ext(const std::string& name_pattern,
                                               const std::string& type_filter,
                                               int offset,
                                               int limit,
                                               int& out_total) override {
        (void)name_pattern;
        (void)type_filter;
        (void)offset;
        (void)limit;
        (void)out_total;
        return {};
      }

      bool update_file_record(const FileRecord& record) override {
        (void)record;
        return false;
      }

      bool store_file_chunks(const std::vector<FileChunkRecord>& chunks) override {
        (void)chunks;
        return false;
      }

      std::vector<FileChunkRecord> get_file_chunks(const std::string& file_hash) override {
        (void)file_hash;
        return {};
      }

      bool chunk_exists(const std::string& chunk_hash) override {
        (void)chunk_hash;
        return false;
      }

      LookupResult<AuthUser> get_auth_user_result(const std::string& username) override {
        (void)username;
        return {};
      }

      bool verify_password(const std::string& username, const std::string& password) override {
        (void)username;
        (void)password;
        return false;
      }

      std::vector<MusicMeta> list_music_library(const std::string& search,
                                                int offset,
                                                int limit,
                                                int& out_total) override {
        (void)search;
        (void)offset;
        (void)limit;
        (void)out_total;
        return {};
      }

      std::optional<MusicMeta> get_music_meta(int64_t music_id) override {
        (void)music_id;
        return {};
      }

      std::optional<MusicMeta> get_music_by_file_id(int64_t file_id) override {
        (void)file_id;
        return {};
      }

      int64_t create_music_meta(const MusicMeta& meta) override {
        (void)meta;
        return 0;
      }

      bool update_music_meta(const MusicMeta& meta) override {
        (void)meta;
        return false;
      }

      bool delete_music_meta(int64_t music_id) override {
        (void)music_id;
        return false;
      }

      MutationResult<std::vector<Playlist>> get_user_playlists(int64_t user_id, int64_t actor_id) override {
        (void)user_id;
        (void)actor_id;
        return {};
      }

      MutationResult<Playlist> create_playlist(const Playlist& playlist, int64_t actor_id) override {
        (void)playlist;
        (void)actor_id;
        return {};
      }

      MutationResult<Playlist> update_playlist(int64_t playlist_id,
                                               int64_t actor_id,
                                               const std::string& name,
                                               const std::string& description) override {
        (void)playlist_id;
        (void)actor_id;
        (void)name;
        (void)description;
        return {status, std::nullopt, detail};
      }

      MutationResult<std::monostate> delete_playlist(int64_t playlist_id, int64_t actor_id) override {
        (void)playlist_id;
        (void)actor_id;
        return {};
      }

      MutationResult<std::vector<PlaylistItem>> get_playlist_items(int64_t playlist_id, int64_t actor_id) override {
        (void)playlist_id;
        (void)actor_id;
        return {};
      }

      MutationResult<std::monostate> add_playlist_item(int64_t playlist_id,
                                                       int64_t actor_id,
                                                       int64_t music_id) override {
        (void)playlist_id;
        (void)actor_id;
        (void)music_id;
        return {};
      }

      MutationResult<std::monostate> remove_playlist_item(int64_t playlist_id,
                                                          int64_t actor_id,
                                                          int64_t music_id) override {
        (void)playlist_id;
        (void)actor_id;
        (void)music_id;
        return {};
      }

      MutationResult<std::monostate> reorder_playlist_items(int64_t playlist_id,
                                                            int64_t actor_id,
                                                            const std::vector<int64_t>& music_ids) override {
        (void)playlist_id;
        (void)actor_id;
        (void)music_ids;
        return {};
      }

      MutationStatus status;
      std::optional<std::string> detail;
    } database(test_case.status, test_case.detail);

    CapturingHttpServer server;
    register_playlist_routes(server, database);
    auto request = authenticated_request(42, UserRole::NORMAL);
    request.path_params["id"] = "7";
    request.body = R"({"name":"renamed","description":"desc"})";
    HttpResponse response;
    server.put_handlers.at("/api/playlists/:id")(request, response);
    EXPECT_EQ(response.status_code, test_case.http_status);
    EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), test_case.code);
  }
}

TEST(Step19PlaylistRouteTest, RejectsInvalidPathsDuplicateFieldsWrongTypesAndIntegerOverflow) {
  MockConnection* connection = nullptr;
  auto database = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  connection->execute_result = 1;
  connection->last_insert_id_value = 7;
  connection->query_hook = [](const std::string& sql,
                              const std::vector<std::string>& params) -> std::optional<QueryResult> {
    if (sql.find("SELECT p.playlist_id") != std::string::npos)
      return QueryResult{.rows = {{"7", "42", "name", "desc", "0", "now"}}};
    if (sql.find("user_playlists") != std::string::npos)
      return QueryResult{.rows = {{"42"}}};
    if (sql.find("FROM users") != std::string::npos)
      return QueryResult{.rows = {{"42"}}};
    if (sql.find("music_meta") != std::string::npos)
      return QueryResult{.rows = {{params.at(0)}}};
    return QueryResult{};
  };
  CapturingHttpServer server;
  register_playlist_routes(server, *database);
  auto request = authenticated_request(42, UserRole::NORMAL);

  for (const std::string id : {"", "0", "-1", "1x", "9223372036854775808"}) {
    request.path_params["id"] = id;
    HttpResponse response;
    EXPECT_NO_THROW(server.del_handlers.at("/api/playlists/:id")(request, response));
    EXPECT_EQ(response.status_code, 400) << id;
    EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "INVALID_REQUEST") << id;
  }

  request.path_params["id"] = "7";
  const std::vector<std::pair<std::string, std::string>> invalid_bodies = {
    {"/api/users/:id/playlists", R"({"name":"first","name":"second"})"},
    {"/api/playlists/:id", R"({"name":"ok","description":"a","description":"b"})"},
    {"/api/playlists/:id/items", R"({"music_id":1,"music_id":2})"},
    {"/api/playlists/:id/items/reorder", R"({"music_ids":[],"music_ids":[]})"},
  };
  for (const auto& [path, body] : invalid_bodies) {
    request.body = body;
    HttpResponse response;
    if (path == "/api/users/:id/playlists") {
      request.path_params["id"] = "42";
      server.post_handlers.at(path)(request, response);
      request.path_params["id"] = "7";
    } else if (path == "/api/playlists/:id/items") {
      server.post_handlers.at(path)(request, response);
    } else {
      server.put_handlers.at(path)(request, response);
    }
    EXPECT_EQ(response.status_code, 400) << path;
    EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "INVALID_REQUEST") << path;
  }

  for (const std::string body : {R"({})", R"({"music_id":"1"})", R"({"music_id":9223372036854775808})"}) {
    request.body = body;
    HttpResponse response;
    EXPECT_NO_THROW(server.post_handlers.at("/api/playlists/:id/items")(request, response));
    EXPECT_EQ(response.status_code, 400) << body;
  }
  for (const std::string body : {R"({})", R"({"music_ids":"1"})", R"({"music_ids":[9223372036854775808]})"}) {
    request.body = body;
    HttpResponse response;
    EXPECT_NO_THROW(server.put_handlers.at("/api/playlists/:id/items/reorder")(request, response));
    EXPECT_EQ(response.status_code, 400) << body;
  }
}

TEST(Step19PlaylistRouteTest, ItemMutationsRequirePositiveRepresentableIntegerIds) {
  MockConnection* connection = nullptr;
  auto database = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  CapturingHttpServer server;
  register_playlist_routes(server, *database);
  auto request = authenticated_request(42, UserRole::NORMAL);
  request.path_params["id"] = "7";

  for (const std::string& body :
       {R"({"music_id":-1})", R"({"music_id":0})", R"({"music_id":1.0})", R"({"music_id":9223372036854775808})"}) {
    request.body = body;
    HttpResponse response;
    server.post_handlers.at("/api/playlists/:id/items")(request, response);
    EXPECT_EQ(response.status_code, 400) << body;
    EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "INVALID_REQUEST") << body;
  }

  for (const std::string& body : {R"({"music_ids":[-1]})",
                                  R"({"music_ids":[0]})",
                                  R"({"music_ids":[1.0]})",
                                  R"({"music_ids":[9223372036854775808]})"}) {
    request.body = body;
    HttpResponse response;
    server.put_handlers.at("/api/playlists/:id/items/reorder")(request, response);
    EXPECT_EQ(response.status_code, 400) << body;
    EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "INVALID_REQUEST") << body;
  }
}

TEST(Step19PlaylistRouteTest, ValidatesUtf8CodePointLengthsAtBothBoundaries) {
  MockConnection* connection = nullptr;
  auto database = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  connection->execute_result = 1;
  connection->query_hook = [](const std::string& sql, const std::vector<std::string>&) -> std::optional<QueryResult> {
    if (sql.find("SELECT p.playlist_id") != std::string::npos)
      return QueryResult{.rows = {{"7", "42", "stored", "stored", "3", "2026-01-02 03:04:05.000000"}}};
    if (sql.find("user_playlists") != std::string::npos)
      return QueryResult{.rows = {{"42"}}};
    return QueryResult{};
  };
  CapturingHttpServer server;
  register_playlist_routes(server, *database);
  auto request = authenticated_request(42, UserRole::NORMAL);
  request.path_params["id"] = "7";
  const std::string chinese = "歌";
  const auto repeat = [](std::string_view value, std::size_t count) {
    std::string result;
    for (std::size_t index = 0; index < count; ++index) result += value;
    return result;
  };

  request.body = nlohmann::json{{"name", std::string(128, 'a')}, {"description", std::string(512, 'b')}}.dump();
  HttpResponse ascii_boundary;
  server.put_handlers.at("/api/playlists/:id")(request, ascii_boundary);
  EXPECT_EQ(ascii_boundary.status_code, 200);

  request.body = nlohmann::json{{"name", chinese + chinese}, {"description", chinese}}.dump();
  HttpResponse utf8_valid;
  server.put_handlers.at("/api/playlists/:id")(request, utf8_valid);
  EXPECT_EQ(utf8_valid.status_code, 200);

  request.body = nlohmann::json{{"name", repeat(chinese, 128)}, {"description", repeat(chinese, 512)}}.dump();
  HttpResponse utf8_boundary;
  server.put_handlers.at("/api/playlists/:id")(request, utf8_boundary);
  EXPECT_EQ(utf8_boundary.status_code, 200);

  for (const auto& payload : {nlohmann::json{{"name", std::string(129, 'a')}, {"description", ""}},
                              nlohmann::json{{"name", "ok"}, {"description", std::string(513, 'b')}},
                              nlohmann::json{{"name", repeat(chinese, 129)}, {"description", ""}},
                              nlohmann::json{{"name", "ok"}, {"description", repeat(chinese, 513)}}}) {
    request.body = payload.dump();
    HttpResponse invalid;
    server.put_handlers.at("/api/playlists/:id")(request, invalid);
    EXPECT_EQ(invalid.status_code, 400);
    EXPECT_EQ(nlohmann::json::parse(invalid.body).at("code"), "INVALID_REQUEST");
  }

  request.body = std::string{R"({"name":")"} + static_cast<char>(0xFF) + R"(","description":""})";
  HttpResponse invalid_utf8;
  EXPECT_NO_THROW(server.put_handlers.at("/api/playlists/:id")(request, invalid_utf8));
  EXPECT_EQ(invalid_utf8.status_code, 400);

  for (const auto& invalid : {std::string{"\x80", 1},
                              std::string{"\xC0\xAF", 2},
                              std::string{"\xED\xA0\x80", 3},
                              std::string{"\xF4\x90\x80\x80", 4}}) {
    request.body = std::string{R"({"name":")"} + invalid + R"(","description":""})";
    HttpResponse invalid_name;
    EXPECT_NO_THROW(server.put_handlers.at("/api/playlists/:id")(request, invalid_name));
    EXPECT_EQ(invalid_name.status_code, 400);

    request.body = std::string{R"({"name":"valid","description":")"} + invalid + R"("})";
    HttpResponse invalid_description;
    EXPECT_NO_THROW(server.put_handlers.at("/api/playlists/:id")(request, invalid_description));
    EXPECT_EQ(invalid_description.status_code, 400);
  }
}

TEST(Step19PlaylistRouteTest, RemoveReturnsEmpty204AndMusicNotFoundIsDistinct) {
  MockConnection* connection = nullptr;
  auto database = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  connection->execute_result = 1;
  connection->query_hook = [](const std::string& sql, const std::vector<std::string>&) -> std::optional<QueryResult> {
    if (sql.find("user_playlists") != std::string::npos)
      return QueryResult{.rows = {{"42"}}};
    if (sql.find("playlist_items") != std::string::npos)
      return QueryResult{.rows = {{"3", "0"}}};
    return QueryResult{};
  };
  CapturingHttpServer server;
  register_playlist_routes(server, *database);
  auto request = authenticated_request(42, UserRole::NORMAL);
  request.path_params["id"] = "7";
  request.path_params["music_id"] = "3";
  HttpResponse removed;
  server.del_handlers.at("/api/playlists/:id/items/:music_id")(request, removed);
  EXPECT_EQ(removed.status_code, 204);
  EXPECT_TRUE(removed.body.empty());

  request.path_params["music_id"] = "99";
  HttpResponse missing_item;
  server.del_handlers.at("/api/playlists/:id/items/:music_id")(request, missing_item);
  EXPECT_EQ(missing_item.status_code, 404);
  EXPECT_EQ(nlohmann::json::parse(missing_item.body).at("code"), "MUSIC_NOT_FOUND");

  request.body = R"({"music_id":99})";
  HttpResponse missing_music;
  server.post_handlers.at("/api/playlists/:id/items")(request, missing_music);
  EXPECT_EQ(missing_music.status_code, 404);
  EXPECT_EQ(nlohmann::json::parse(missing_music.body).at("code"), "MUSIC_NOT_FOUND");
}

TEST(Step19PlaylistRouteTest, CreateAndListReturnCompletePlaylistObjects) {
  MockConnection* connection = nullptr;
  auto database = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  connection->execute_result = 1;
  connection->last_insert_id_value = 7;
  connection->query_hook = [](const std::string& sql, const std::vector<std::string>&) -> std::optional<QueryResult> {
    if (sql.find("FROM users") != std::string::npos)
      return QueryResult{.rows = {{"42"}}};
    return QueryResult{.rows = {{"7", "42", "完整歌单", "描述", "4", "2026-07-28 01:02:03.000000"}}};
  };
  CapturingHttpServer server;
  register_playlist_routes(server, *database);
  auto request = authenticated_request(42, UserRole::NORMAL);
  request.path_params["id"] = "42";

  HttpResponse listed;
  server.get_handlers.at("/api/users/:id/playlists")(request, listed);
  ASSERT_EQ(listed.status_code, 200);
  const auto list_item = nlohmann::json::parse(listed.body).at("playlists").at(0);
  EXPECT_EQ(list_item.at("id"), 7);
  EXPECT_EQ(list_item.at("user_id"), 42);
  EXPECT_EQ(list_item.at("item_count"), 4);
  EXPECT_EQ(list_item.at("created_at"), "2026-07-28T01:02:03.000000Z");

  request.body = R"({"name":"完整歌单","description":"描述"})";
  HttpResponse created;
  server.post_handlers.at("/api/users/:id/playlists")(request, created);
  ASSERT_EQ(created.status_code, 201);
  const auto created_body = nlohmann::json::parse(created.body);
  EXPECT_EQ(created_body.at("id"), 7);
  EXPECT_EQ(created_body.at("user_id"), 42);
  EXPECT_EQ(created_body.at("item_count"), 4);
  EXPECT_EQ(created_body.at("created_at"), "2026-07-28T01:02:03.000000Z");
}

TEST(Step19PlaylistRouteTest, SerializesPlaylistAndItemTimesAsRfc3339Utc) {
  MockConnection* connection = nullptr;
  auto database = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  connection->execute_hook = [](const std::string&, const std::vector<std::string>&) {
    return std::optional<int64_t>{1};
  };
  connection->query_hook = [](const std::string& sql, const std::vector<std::string>&) -> std::optional<QueryResult> {
    if (sql.find("FROM user_playlists p WHERE p.user_id") != std::string::npos) {
      return QueryResult{.rows = {{"7", "42", "时间歌单", "描述", "1", "2026-07-28 01:02:03.000000"}}};
    }
    if (sql.find("SELECT user_id FROM user_playlists WHERE playlist_id") != std::string::npos) {
      return QueryResult{.rows = {{"42"}}};
    }
    if (sql.find("FROM playlist_items pi") != std::string::npos) {
      return QueryResult{.rows = {{"11", "7", "5", "0", "2026-07-28 01:02:04.000000", "歌曲", "艺人", "hash-5"}}};
    }
    return QueryResult{};
  };
  CapturingHttpServer server;
  register_playlist_routes(server, *database);
  auto request = authenticated_request(42, UserRole::NORMAL);
  request.path_params["id"] = "42";
  HttpResponse playlists;

  server.get_handlers.at("/api/users/:id/playlists")(request, playlists);

  ASSERT_EQ(playlists.status_code, 200);
  EXPECT_EQ(nlohmann::json::parse(playlists.body).at("playlists").at(0).at("created_at"),
            "2026-07-28T01:02:03.000000Z");

  request.path_params["id"] = "7";
  HttpResponse items;
  server.get_handlers.at("/api/playlists/:id/items")(request, items);

  ASSERT_EQ(items.status_code, 200);
  EXPECT_EQ(nlohmann::json::parse(items.body).at("items").at(0).at("added_at"), "2026-07-28T01:02:04.000000Z");
}

TEST(Step19PlaylistRouteTest, RejectsInvalidPersistedPlaylistTime) {
  MockConnection* connection = nullptr;
  auto database = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  connection->query_result = QueryResult{.rows = {{"7", "42", "时间歌单", "描述", "1", "not-a-datetime"}}};
  CapturingHttpServer server;
  register_playlist_routes(server, *database);
  auto request = authenticated_request(42, UserRole::NORMAL);
  request.path_params["id"] = "42";
  HttpResponse response;

  server.get_handlers.at("/api/users/:id/playlists")(request, response);

  EXPECT_EQ(response.status_code, 422);
  EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "INVALID_STATE");
}

TEST(Step19PlaylistRouteTest, RemoveStrictlyRejectsInvalidMusicPathIds) {
  MockConnection* connection = nullptr;
  auto database = make_database_pool(connection);
  CapturingHttpServer server;
  register_playlist_routes(server, *database);
  auto request = authenticated_request(42, UserRole::NORMAL);
  request.path_params["id"] = "7";
  for (const std::string music_id : {"", "0", "-1", "1x", "9223372036854775808"}) {
    request.path_params["music_id"] = music_id;
    HttpResponse response;
    EXPECT_NO_THROW(server.del_handlers.at("/api/playlists/:id/items/:music_id")(request, response));
    EXPECT_EQ(response.status_code, 400) << music_id;
    EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "INVALID_REQUEST") << music_id;
  }
}

TEST(Step19PlaylistRouteTest, ReorderDatabaseConflictsMapToStable409) {
  for (const std::string body : {R"({"music_ids":[3,3]})", R"({"music_ids":[3]})", R"({"music_ids":[3,5,9]})"}) {
    MockConnection* connection = nullptr;
    auto database = make_database_pool(connection);
    ASSERT_NE(connection, nullptr);
    connection->execute_result = 1;
    connection->query_hook = [](const std::string& sql, const std::vector<std::string>&) -> std::optional<QueryResult> {
      if (sql.find("user_playlists") != std::string::npos)
        return QueryResult{.rows = {{"42"}}};
      return QueryResult{.rows = {{"3", "0"}, {"5", "1"}}};
    };
    CapturingHttpServer server;
    register_playlist_routes(server, *database);
    auto request = authenticated_request(42, UserRole::NORMAL);
    request.path_params["id"] = "7";
    request.body = body;
    HttpResponse response;

    server.put_handlers.at("/api/playlists/:id/items/reorder")(request, response);

    EXPECT_EQ(response.status_code, 409) << body;
    EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "PLAYLIST_ORDER_CONFLICT") << body;
  }
}

TEST(Step19PlaylistRouteTest, EveryJsonEndpointRejectsUnknownTopLevelFields) {
  MockConnection* connection = nullptr;
  auto database = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  connection->execute_result = 1;
  connection->last_insert_id_value = 7;
  connection->query_hook = [](const std::string& sql,
                              const std::vector<std::string>& params) -> std::optional<QueryResult> {
    if (sql.find("FROM users") != std::string::npos)
      return QueryResult{.rows = {{"42"}}};
    if (sql.find("SELECT p.playlist_id") != std::string::npos)
      return QueryResult{.rows = {{"7", "42", "name", "desc", "0", "now"}}};
    if (sql.find("user_playlists") != std::string::npos)
      return QueryResult{.rows = {{"42"}}};
    if (sql.find("music_meta") != std::string::npos)
      return QueryResult{.rows = {{params.at(0)}}};
    return QueryResult{};
  };
  CapturingHttpServer server;
  register_playlist_routes(server, *database);
  auto request = authenticated_request(42, UserRole::NORMAL);

  struct Case {
    std::string path;
    std::string body;
    bool post;
  };

  for (const Case& test_case : {
         Case{"/api/users/:id/playlists", R"({"name":"name","metadata":{"source":"x"}})", true},
         Case{"/api/playlists/:id", R"({"name":"name","extra":1})", false},
         Case{"/api/playlists/:id/items", R"({"music_id":9,"extra":1})", true},
         Case{"/api/playlists/:id/items/reorder", R"({"music_ids":[],"metadata":{}})", false},
       }) {
    request.path_params["id"] = test_case.path.starts_with("/api/users") ? "42" : "7";
    request.body = test_case.body;
    HttpResponse response;
    if (test_case.post)
      server.post_handlers.at(test_case.path)(request, response);
    else
      server.put_handlers.at(test_case.path)(request, response);
    EXPECT_EQ(response.status_code, 400) << test_case.path;
    EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "INVALID_REQUEST") << test_case.path;
  }
}

TEST(Step19PlaylistJsonTest, ScopesDuplicateKeysPerObjectAtEveryDepth) {
  EXPECT_TRUE(parse_playlist_json_object(R"({"left":{"id":1},"right":{"id":2}})").has_value());
  EXPECT_TRUE(parse_playlist_json_object(R"({"outer":{"left":{"id":1},"right":{"id":2}}})").has_value());
  EXPECT_FALSE(parse_playlist_json_object(R"({"outer":{"id":1,"id":2}})").has_value());
  EXPECT_FALSE(parse_playlist_json_object(R"({"outer":{"inner":{"id":1,"id":2}}})").has_value());
  EXPECT_FALSE(parse_playlist_json_object(R"({"name":"a","name":"b"})").has_value());
}

TEST(Step19PlaylistRouteTest, CreateMapsMissingTargetUserPrecisely) {
  MockConnection* connection = nullptr;
  auto database = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  connection->execute_result = 1;
  connection->query_result = QueryResult{};
  CapturingHttpServer server;
  register_playlist_routes(server, *database);
  auto request = authenticated_request(42, UserRole::NORMAL);
  request.path_params["id"] = "42";
  request.body = R"({"name":"name"})";
  HttpResponse response;

  server.post_handlers.at("/api/users/:id/playlists")(request, response);

  EXPECT_EQ(response.status_code, 404);
  EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "USER_NOT_FOUND");
}

// ============================================================
// T1: Content-Type 自动检测
// ============================================================
TEST(Step16UtilTest, DetectContentType) {
  auto detect = [](const std::string& name) -> std::string {
    auto dot = name.rfind('.');
    if (dot == std::string::npos)
      return "application/octet-stream";
    auto ext = name.substr(dot);
    if (ext == ".mp3")
      return "audio/mpeg";
    if (ext == ".ogg")
      return "audio/ogg";
    if (ext == ".flac")
      return "audio/flac";
    if (ext == ".wav")
      return "audio/wav";
    return "application/octet-stream";
  };

  EXPECT_EQ(detect("song.mp3"), "audio/mpeg");
  EXPECT_EQ(detect("track.ogg"), "audio/ogg");
  EXPECT_EQ(detect("album.flac"), "audio/flac");
  EXPECT_EQ(detect("sample.wav"), "audio/wav");
  EXPECT_EQ(detect("file.bin"), "application/octet-stream");
  EXPECT_EQ(detect("noext"), "application/octet-stream");
}

// ============================================================
// T2: 加盐哈希密码
// ============================================================
TEST(Step16UtilTest, SaltedHashPassword) {
  auto salt = generate_salt();
  EXPECT_EQ(salt.size(), 32); // 16 bytes → 32 hex chars

  auto hash1 = hash_password("test123", salt);
  EXPECT_FALSE(hash1.empty());
  EXPECT_EQ(hash1.size(), 64); // SHA-256 → 64 hex chars

  auto hash2 = hash_password("test123", salt);
  EXPECT_EQ(hash1, hash2);

  auto hash3 = hash_password("test123", "different_salt_abc1234567890");
  EXPECT_NE(hash1, hash3);
}

// ============================================================
// T3: Range Parser（音频流播依赖）
// ============================================================
TEST(Step16UtilTest, RangeParser) {
  auto range = parse_range_header("bytes=0-99", 1000);
  EXPECT_TRUE(range.valid);
  EXPECT_TRUE(range.satisfiable);
  ASSERT_EQ(range.ranges.size(), 1U);
  EXPECT_EQ(range.ranges[0].start, 0U);
  EXPECT_EQ(range.ranges[0].end, 100U);

  auto range2 = parse_range_header("bytes=-", 1000);
  EXPECT_FALSE(range2.valid);

  // 206 headers
  HttpResponse resp;
  resp.set_status(200, "OK");
  build_206_headers(resp, range, 1000);
  EXPECT_FALSE(resp.headers.empty());

  // 416
  HttpResponse resp416;
  build_416_response(resp416, 1000);
  EXPECT_EQ(resp416.status_code, 416);
}

// ============================================================
// T4: CORS headers
// ============================================================
TEST(Step16UtilTest, CorsHeaders) {
  HttpResponse resp;
  resp.set_status(200, "OK");

  auto cors = [](HttpResponse& r) {
    r.set_header("Access-Control-Allow-Origin", "*");
    r.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    r.set_header("Access-Control-Allow-Headers", "Authorization, Content-Type, Range");
    r.set_header("Access-Control-Expose-Headers", "Content-Range, Accept-Ranges, Content-Disposition");
    r.set_header("Access-Control-Max-Age", "86400");
  };
  cors(resp);

  EXPECT_EQ(resp.headers["Access-Control-Allow-Origin"], "*");
  EXPECT_EQ(resp.headers["Access-Control-Max-Age"], "86400");
}

// ============================================================
// T5: Token 生成 + 验证
// ============================================================
TEST(Step16ApiTest, TokenGenerationValidation) {
  auto factory = []() -> std::unique_ptr<IConnection> {
    auto connection = std::make_unique<MockConnection>();
    connection->query_hook = [](const std::string&, const std::vector<std::string>&) {
      return std::optional<QueryResult>{QueryResult{
        .columns = {"user_id", "username", "password_hash", "salt", "role", "email", "vip_expires_at", "created_at"},
        .rows = {{"42", "testuser", "hash", "salt", "1", "test@example.com", "", "2026-01-02 03:04:05.000000"}},
      }};
    };
    return connection;
  };

  DatabasePool pool(factory);
  DbConfig cfg;
  cfg.pool_size = 1;
  cfg.connect_timeout_ms = 500;
  ASSERT_TRUE(pool.init(cfg));

  auto auth = create_auth_service(pool, "test-secret");

  AuthUser au;
  au.user_id = 42;
  au.username = "testuser";
  au.role = UserRole::NORMAL;

  auto token = auth->generate_token(au);
  EXPECT_FALSE(token.empty());

  // Validate token directly
  auto validated = auth->validate_token(token);
  EXPECT_EQ(validated.status, TokenValidationStatus::AUTHENTICATED);
  EXPECT_EQ(validated.identity.user_id, 42);
  EXPECT_EQ(validated.identity.role, UserRole::NORMAL);
  ASSERT_TRUE(validated.profile);
  EXPECT_EQ(validated.profile->email, "test@example.com");

  // Validate without token
  auto empty = auth->validate_token("");
  EXPECT_EQ(empty.status, TokenValidationStatus::INVALID);
  EXPECT_EQ(empty.identity.role, UserRole::GUEST);

  // Validate via AuthMiddleware
  HttpRequest req;
  req.headers["Authorization"] = "Bearer " + token;
  AuthMiddleware::apply(*auth, req);
  EXPECT_EQ(req.auth_status, TokenValidationStatus::AUTHENTICATED);
  EXPECT_EQ(req.auth_user.user_id, 42);
  EXPECT_EQ(req.auth_user.role, UserRole::NORMAL);
  ASSERT_TRUE(req.auth_profile);
  EXPECT_EQ(req.auth_profile->created_at, "2026-01-02 03:04:05.000000");

  HttpRequest req2;
  AuthMiddleware::apply(*auth, req2);
  EXPECT_EQ(req2.auth_status, TokenValidationStatus::INVALID);
  EXPECT_EQ(req2.auth_user.role, UserRole::GUEST);
  EXPECT_FALSE(req2.auth_profile);
}

TEST(Step16ApiTest, AuthMiddlewarePreservesStorageError) {
  class StorageErrorAuthService final : public IAuthService {
  public:
    TokenValidationResult validate_token(const std::string& token) override {
      (void)token;
      return {TokenValidationStatus::STORAGE_ERROR, {}, {}};
    }

    std::string generate_token(const AuthUser& user) override {
      (void)user;
      return {};
    }

    AuthenticationResult authenticate(const std::string& username, const std::string& password) override {
      (void)username;
      (void)password;
      return {};
    }
  } auth;

  HttpRequest request;
  request.headers["Authorization"] = "Bearer valid-token";
  AuthMiddleware::apply(auth, request);

  EXPECT_EQ(request.auth_status, TokenValidationStatus::STORAGE_ERROR);
  EXPECT_EQ(request.auth_user.role, UserRole::GUEST);
}

TEST(Step16ApiTest, AuthMiddlewareConvertsServiceExceptionToStorageError) {
  class ThrowingAuthService final : public IAuthService {
  public:
    TokenValidationResult validate_token(const std::string& token) override {
      (void)token;
      throw std::runtime_error("auth failure");
    }

    std::string generate_token(const AuthUser& user) override {
      (void)user;
      return {};
    }

    AuthenticationResult authenticate(const std::string& username, const std::string& password) override {
      (void)username;
      (void)password;
      return {};
    }
  } auth;

  HttpRequest request;
  request.headers["Authorization"] = "Bearer valid-token";

  EXPECT_NO_THROW(AuthMiddleware::apply(auth, request));
  EXPECT_EQ(request.auth_status, TokenValidationStatus::STORAGE_ERROR);
  EXPECT_EQ(request.auth_user.role, UserRole::GUEST);
}

TEST(Step16ApiTest, AuthUserJsonUsesEffectiveIdentityAndIncludesCompleteContract) {
  User user;
  user.user_id = 42;
  user.username = "testuser";
  user.email = "test@example.com";
  user.role = UserRole::VIP;
  user.vip_expires_at = std::chrono::system_clock::time_point{std::chrono::seconds{2'000'000'000}};
  user.created_at = "2026-01-02 03:04:05.000000";
  const auto identity =
    make_effective_identity(user, std::chrono::system_clock::time_point{std::chrono::seconds{2'000'000'001}});

  const auto json = serialize_auth_user(user, identity);

  EXPECT_EQ(json.at("user_id"), 42);
  EXPECT_EQ(json.at("username"), "testuser");
  EXPECT_EQ(json.at("email"), "test@example.com");
  EXPECT_EQ(json.at("role"), "NORMAL");
  EXPECT_EQ(json.at("vip_status"), "EXPIRED");
  EXPECT_EQ(json.at("vip_expires_at"), "2033-05-18T03:33:20.000000Z");
  EXPECT_EQ(json.at("created_at"), "2026-01-02T03:04:05.000000Z");
  EXPECT_EQ(json.at("capabilities"), nlohmann::json::array({"USE_AUTHENTICATED_FEATURES"}));
}

TEST(Step16ApiTest, AuthResponseNestsUser) {
  User user;
  user.user_id = 7;
  user.username = "nested";
  user.email = "nested@example.com";
  user.role = UserRole::NORMAL;
  user.created_at = "2026-02-03 04:05:06.000000";
  const auto identity = make_effective_identity(user, std::chrono::system_clock::now());

  const auto response = serialize_auth_response("signed-token", user, identity);

  EXPECT_EQ(response.at("token"), "signed-token");
  ASSERT_TRUE(response.contains("user"));
  EXPECT_EQ(response.at("user").at("user_id"), 7);
  EXPECT_EQ(response.at("user").at("created_at"), "2026-02-03T04:05:06.000000Z");
  EXPECT_FALSE(response.contains("user_id"));
  EXPECT_FALSE(response.contains("role"));
}

TEST(Step16ApiTest, AuthUserJsonRejectsInvalidCreatedAt) {
  User user;
  user.user_id = 9;
  user.username = "invalid-time";
  user.email = "invalid@example.com";
  user.role = UserRole::NORMAL;
  user.created_at = "not-a-datetime";
  const auto identity = make_effective_identity(user, std::chrono::system_clock::now());

  EXPECT_THROW(static_cast<void>(serialize_auth_user(user, identity)), std::runtime_error);
}

TEST(Step16ApiTest, RegistrationRejectsPrivilegeFields) {
  EXPECT_FALSE(has_forbidden_registration_fields(
    nlohmann::json{{"username", "safe"}, {"password", "secret"}, {"email", "safe@example.com"}}));
  EXPECT_TRUE(has_forbidden_registration_fields(nlohmann::json{{"role", "ADMIN"}}));
  EXPECT_TRUE(has_forbidden_registration_fields(nlohmann::json{{"vip_status", "ACTIVE"}}));
  EXPECT_TRUE(has_forbidden_registration_fields(nlohmann::json{{"vip_expires_at", "2099-01-01T00:00:00Z"}}));
  EXPECT_TRUE(has_forbidden_registration_fields(nlohmann::json{{"admin", true}}));
}

TEST(Step16ApiTest, RegistrationAndLoginRejectDuplicateUnknownAndWrongTypeFieldsBeforePersistence) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  int query_count = 0;
  connection->query_hook = [&query_count](const std::string&, const std::vector<std::string>&) {
    ++query_count;
    return std::optional<QueryResult>{QueryResult{}};
  };
  auto auth = create_auth_service(*pool, "test-secret");
  CapturingHttpServer server;
  register_auth_routes(server, *pool, *auth);

  const auto expect_invalid = [&](const std::string& path, const std::string& body) {
    HttpRequest request;
    request.body = body;
    HttpResponse response;
    server.post_handlers.at(path)(request, response);
    EXPECT_EQ(response.status_code, 400) << path << ' ' << body;
    EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "INVALID_REQUEST") << path << ' ' << body;
  };

  for (const std::string& body : {R"({"username":"alice","username":"bob","password":"secret1"})",
                                  R"({"username":"alice","password":"secret1","extra":true})",
                                  R"({"username":7,"password":"secret1"})"}) {
    expect_invalid("/api/auth/register", body);
  }
  for (const std::string& body : {R"({"username":"alice","username":"bob","password":"secret1"})",
                                  R"({"username":"alice","password":"secret1","extra":true})",
                                  R"({"username":"alice","password":false})"}) {
    expect_invalid("/api/auth/login", body);
  }
  EXPECT_EQ(query_count, 0);
}

TEST(Step16ApiTest, RegistrationClassifiesPrivilegeFieldsBeforeUnknownFieldValidation) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  int query_count = 0;
  connection->query_hook = [&query_count](const std::string&, const std::vector<std::string>&) {
    ++query_count;
    return std::optional<QueryResult>{QueryResult{}};
  };
  auto auth = create_auth_service(*pool, "test-secret");
  CapturingHttpServer server;
  register_auth_routes(server, *pool, *auth);

  const auto invoke = [&](const std::string& body) {
    HttpRequest request;
    request.body = body;
    HttpResponse response;
    server.post_handlers.at("/api/auth/register")(request, response);
    return response;
  };

  for (const std::string& body : {R"({"username":"alice","password":"secret1","role":"ADMIN"})",
                                  R"({"username":"alice","password":"secret1","vip_status":"ACTIVE"})",
                                  R"({"username":"alice","password":"secret1","vip_expires_at":"2099-01-01"})"}) {
    const auto response = invoke(body);
    EXPECT_EQ(response.status_code, 400) << body;
    EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "REGISTRATION_FIELD_FORBIDDEN") << body;
  }

  const auto unknown = invoke(R"({"username":"alice","password":"secret1","extra":true})");
  EXPECT_EQ(unknown.status_code, 400);
  EXPECT_EQ(nlohmann::json::parse(unknown.body).at("code"), "INVALID_REQUEST");
  EXPECT_EQ(query_count, 0);
}

TEST(Step16ApiTest, ProtectedMeRouteMapsAuthenticationStorageErrorToPersistenceError) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  auto auth = create_auth_service(*pool, "test-secret");
  CapturingHttpServer server;
  register_auth_routes(server, *pool, *auth);

  HttpRequest request;
  request.auth_status = TokenValidationStatus::STORAGE_ERROR;
  HttpResponse response;
  server.get_handlers.at("/api/auth/me")(request, response);

  EXPECT_EQ(response.status_code, 500);
  EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "PERSISTENCE_ERROR");
}

TEST(Step16ApiTest, ProtectedMeRouteReusesAuthenticatedProfileWithoutSecondDatabaseLookup) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  int query_count = 0;
  connection->query_hook = [&query_count](const std::string&, const std::vector<std::string>&) {
    ++query_count;
    return std::optional<QueryResult>{
      QueryResult{.rows = {
                    {"42", "alice", "hash", "salt", "1", "alice@example.com", "", "2026-01-02 03:04:05.000000"},
                  }}};
  };
  auto auth = create_auth_service(*pool, "test-secret");
  CapturingHttpServer server;
  register_auth_routes(server, *pool, *auth);

  const AuthUser token_user{42, "alice", UserRole::NORMAL};
  HttpRequest request;
  request.headers["Authorization"] = "Bearer " + auth->generate_token(token_user);
  AuthMiddleware::apply(*auth, request);
  ASSERT_EQ(request.auth_status, TokenValidationStatus::AUTHENTICATED);
  ASSERT_TRUE(request.auth_profile);

  HttpResponse response;
  server.get_handlers.at("/api/auth/me")(request, response);

  EXPECT_EQ(response.status_code, 200);
  EXPECT_EQ(query_count, 1);
  const auto payload = nlohmann::json::parse(response.body);
  EXPECT_EQ(payload.at("username"), "alice");
  EXPECT_EQ(payload.at("email"), "alice@example.com");
  EXPECT_EQ(payload.at("created_at"), "2026-01-02T03:04:05.000000Z");
}

TEST(Step16ApiTest, ProtectedMeRouteKeepsDisappearedUserAsAuthRequiredAcrossAuthenticationPaths) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  connection->query_result = QueryResult{};
  auto auth = create_auth_service(*pool, "test-secret");
  CapturingHttpServer server;
  register_auth_routes(server, *pool, *auth);
  AuthUser token_user;
  token_user.user_id = 42;
  token_user.username = "deleted";
  token_user.role = UserRole::NORMAL;
  HttpRequest middleware_request;
  middleware_request.headers["Authorization"] = "Bearer " + auth->generate_token(token_user);
  AuthMiddleware::apply(*auth, middleware_request);
  ASSERT_EQ(middleware_request.auth_status, TokenValidationStatus::USER_NOT_FOUND);
  HttpResponse middleware_response;
  server.get_handlers.at("/api/auth/me")(middleware_request, middleware_response);
  EXPECT_EQ(middleware_response.status_code, 401);
  EXPECT_EQ(nlohmann::json::parse(middleware_response.body).at("code"), "AUTH_REQUIRED");

  HttpResponse direct_response;
  server.get_handlers.at("/api/auth/me")(authenticated_request(42, UserRole::NORMAL), direct_response);

  EXPECT_EQ(direct_response.status_code, 401);
  EXPECT_EQ(nlohmann::json::parse(direct_response.body).at("code"), "AUTH_REQUIRED");
}

TEST(Step16ApiTest, ProfileGetRequiresAuthenticationOwnerAndReturnsCompleteAuthUser) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  connection->query_result =
    QueryResult{.rows = {
                  {"42", "alice", "hash", "salt", "1", "alice@example.com", "", "2026-01-02 03:04:05.000000"},
                }};
  auto auth = create_auth_service(*pool, "test-secret");
  CapturingHttpServer server;
  register_auth_routes(server, *pool, *auth);

  ASSERT_TRUE(server.get_handlers.contains("/api/users/:id"));
  const auto invoke = [&](HttpRequest request) {
    request.path_params["id"] = "42";
    HttpResponse response;
    server.get_handlers.at("/api/users/:id")(request, response);
    return response;
  };

  EXPECT_EQ(invoke(HttpRequest{}).status_code, 401);
  const auto forbidden = invoke(authenticated_request(7, UserRole::ADMIN));
  EXPECT_EQ(forbidden.status_code, 403);
  EXPECT_EQ(nlohmann::json::parse(forbidden.body).at("code"), "PROFILE_OWNER_REQUIRED");

  const auto response = invoke(authenticated_request(42, UserRole::NORMAL));
  ASSERT_EQ(response.status_code, 200);
  const auto body = nlohmann::json::parse(response.body);
  EXPECT_EQ(body.at("user_id"), 42);
  EXPECT_EQ(body.at("username"), "alice");
  EXPECT_EQ(body.at("email"), "alice@example.com");
  EXPECT_EQ(body.at("role"), "NORMAL");
  EXPECT_EQ(body.at("vip_status"), "NONE");
  EXPECT_TRUE(body.at("vip_expires_at").is_null());
  EXPECT_EQ(body.at("capabilities"), nlohmann::json::array({"USE_AUTHENTICATED_FEATURES"}));
  EXPECT_EQ(body.at("created_at"), "2026-01-02T03:04:05.000000Z");
}

TEST(Step16ApiTest, ProfileRoutesMapDisappearedAuthenticatedUserToAuthRequired) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  connection->query_result = QueryResult{};
  auto auth = create_auth_service(*pool, "test-secret");
  CapturingHttpServer server;
  register_auth_routes(server, *pool, *auth);

  auto request = authenticated_request(42, UserRole::NORMAL);
  request.path_params["id"] = "42";
  HttpResponse get_response;
  server.get_handlers.at("/api/users/:id")(request, get_response);
  EXPECT_EQ(get_response.status_code, 401);
  EXPECT_EQ(nlohmann::json::parse(get_response.body).at("code"), "AUTH_REQUIRED");

  request.body = R"({"email":"new@example.com"})";
  HttpResponse put_response;
  server.put_handlers.at("/api/users/:id")(request, put_response);
  EXPECT_EQ(put_response.status_code, 401);
  EXPECT_EQ(nlohmann::json::parse(put_response.body).at("code"), "AUTH_REQUIRED");
}

TEST(Step16ApiTest, ProfilePutStrictlyRejectsInvalidTopLevelBodies) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  connection->query_result =
    QueryResult{.rows = {
                  {"42", "alice", "hash", "salt", "1", "alice@example.com", "", "2026-01-02 03:04:05.000000"},
                }};
  auto auth = create_auth_service(*pool, "test-secret");
  CapturingHttpServer server;
  register_auth_routes(server, *pool, *auth);

  ASSERT_TRUE(server.put_handlers.contains("/api/users/:id"));
  HttpResponse unauthenticated;
  HttpRequest unauthenticated_request;
  unauthenticated_request.path_params["id"] = "42";
  unauthenticated_request.body = R"({"email":"new@example.com"})";
  server.put_handlers.at("/api/users/:id")(unauthenticated_request, unauthenticated);
  EXPECT_EQ(unauthenticated.status_code, 401);

  auto other_user_request = authenticated_request(7, UserRole::ADMIN);
  other_user_request.path_params["id"] = "42";
  other_user_request.body = R"({"email":"new@example.com"})";
  HttpResponse forbidden;
  server.put_handlers.at("/api/users/:id")(other_user_request, forbidden);
  EXPECT_EQ(forbidden.status_code, 403);
  EXPECT_EQ(nlohmann::json::parse(forbidden.body).at("code"), "PROFILE_OWNER_REQUIRED");

  const std::vector<std::string> invalid_bodies = {
    R"({)",
    R"([])",
    R"({})",
    R"({"email":"first@example.com","email":"second@example.com"})",
    R"({"unknown":true})",
    R"({"email":7})",
    R"({"email":""})",
    R"({"email":"invalid"})",
    R"({"password":false})",
    R"({"password":""})",
    R"({"password":"12345"})",
    R"({"password":")" + std::string(129, 'x') + R"("})",
    R"({"role":"ADMIN"})",
    R"({"capabilities":["DELETE_ANY_FILE"]})",
    R"({"vip_status":"ACTIVE"})",
    R"({"vip_expires_at":"2099-01-01T00:00:00Z"})",
  };
  for (const auto& body : invalid_bodies) {
    auto request = authenticated_request(42, UserRole::NORMAL);
    request.path_params["id"] = "42";
    request.body = body;
    HttpResponse response;
    server.put_handlers.at("/api/users/:id")(request, response);
    EXPECT_EQ(response.status_code, 400) << body;
    EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "INVALID_REQUEST") << body;
  }
  EXPECT_EQ(connection->last_sql.find("UPDATE users"), std::string::npos);
}

TEST(Step16ApiTest, ProfilePutUpdatesAllowedFieldsAndReturnsCompleteAuthUser) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  connection->query_hook = [](const std::string&, const std::vector<std::string>&) {
    return std::optional<QueryResult>{QueryResult{
      .rows = {{"42", "alice", "old-hash", "old-salt", "1", "new@example.com", "", "2026-01-02 03:04:05.000000"}}}};
  };
  connection->execute_result = 1;
  auto auth = create_auth_service(*pool, "test-secret");
  CapturingHttpServer server;
  register_auth_routes(server, *pool, *auth);
  ASSERT_TRUE(server.put_handlers.contains("/api/users/:id"));
  auto request = authenticated_request(42, UserRole::NORMAL);
  request.path_params["id"] = "42";
  request.body = R"({"email":"new@example.com","password":"secure-password"})";
  HttpResponse response;

  server.put_handlers.at("/api/users/:id")(request, response);

  ASSERT_EQ(response.status_code, 200);
  const auto body = nlohmann::json::parse(response.body);
  EXPECT_EQ(body.at("user_id"), 42);
  EXPECT_EQ(body.at("username"), "alice");
  EXPECT_EQ(body.at("email"), "new@example.com");
  EXPECT_EQ(body.at("role"), "NORMAL");
  EXPECT_EQ(body.at("vip_status"), "NONE");
  EXPECT_TRUE(body.at("vip_expires_at").is_null());
  EXPECT_EQ(body.at("capabilities"), nlohmann::json::array({"USE_AUTHENTICATED_FEATURES"}));
  EXPECT_EQ(body.at("created_at"), "2026-01-02T03:04:05.000000Z");
}

TEST(Step16ApiTest, ProfilePutUpdatesOnlyRequestedColumnsBeforeReadingCurrentAuthUser) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  int query_count = 0;
  std::vector<std::pair<std::string, std::vector<std::string>>> executions;
  connection->query_hook = [&query_count](const std::string&, const std::vector<std::string>&) {
    ++query_count;
    return std::optional<QueryResult>{QueryResult{
      .rows = {
        {"42", "alice", "stored-hash", "stored-salt", "1", "new@example.com", "", "2026-01-02 03:04:05.000000"}}}};
  };
  connection->execute_hook = [&executions](const std::string& sql, const std::vector<std::string>& params) {
    executions.emplace_back(sql, params);
    return std::optional<int64_t>{1};
  };
  auto auth = create_auth_service(*pool, "test-secret");
  CapturingHttpServer server;
  register_auth_routes(server, *pool, *auth);
  auto request = authenticated_request(42, UserRole::NORMAL);
  request.path_params["id"] = "42";
  request.body = R"({"email":"new@example.com"})";
  HttpResponse response;

  server.put_handlers.at("/api/users/:id")(request, response);

  ASSERT_EQ(response.status_code, 200);
  EXPECT_EQ(nlohmann::json::parse(response.body).at("email"), "new@example.com");
  EXPECT_EQ(query_count, 1);
  std::string update_sql;
  std::vector<std::string> update_params;
  for (const auto& [sql, params] : executions) {
    if (sql.starts_with("UPDATE users SET")) {
      update_sql = sql;
      update_params = params;
      break;
    }
  }
  ASSERT_FALSE(update_sql.empty());
  EXPECT_NE(update_sql.find("email = ?"), std::string::npos);
  EXPECT_EQ(update_sql.find("password_hash"), std::string::npos);
  EXPECT_EQ(update_params, (std::vector<std::string>{"new@example.com", "42"}));
}

TEST(Step16ApiTest, ProfilePutReturnsCurrentPersistedUserAfterConcurrentVipChange) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  int lookup_count = 0;
  connection->query_hook = [&lookup_count](const std::string&, const std::vector<std::string>&) {
    ++lookup_count;
    return std::optional<QueryResult>{QueryResult{.rows = {{"42",
                                                            "alice",
                                                            "new-hash",
                                                            "new-salt",
                                                            "2",
                                                            "new@example.com",
                                                            "2099-01-01 00:00:00.000000",
                                                            "2026-01-02 03:04:05.000000"}}}};
  };
  connection->execute_result = 1;
  auto auth = create_auth_service(*pool, "test-secret");
  CapturingHttpServer server;
  register_auth_routes(server, *pool, *auth);
  auto request = authenticated_request(42, UserRole::NORMAL);
  request.path_params["id"] = "42";
  request.body = R"({"email":"new@example.com"})";
  HttpResponse response;

  server.put_handlers.at("/api/users/:id")(request, response);

  ASSERT_EQ(response.status_code, 200);
  const auto body = nlohmann::json::parse(response.body);
  EXPECT_EQ(lookup_count, 1);
  EXPECT_EQ(body.at("email"), "new@example.com");
  EXPECT_EQ(body.at("role"), "VIP");
  EXPECT_EQ(body.at("vip_status"), "ACTIVE");
  EXPECT_EQ(body.at("vip_expires_at"), "2099-01-01T00:00:00.000000Z");
  EXPECT_EQ(body.at("capabilities"), nlohmann::json::array({"USE_AUTHENTICATED_FEATURES", "USE_VIP_BENEFITS"}));
}

TEST(Step16ApiTest, LoginRouteMapsNonThrowingDatabaseFailureToPersistenceError) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  connection->query_result = std::nullopt;
  auto auth = create_auth_service(*pool, "test-secret");
  CapturingHttpServer server;
  register_auth_routes(server, *pool, *auth);

  HttpRequest request;
  request.body = R"({"username":"alice","password":"secret1"})";
  HttpResponse response;
  server.post_handlers.at("/api/auth/login")(request, response);

  EXPECT_EQ(response.status_code, 500);
  EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "PERSISTENCE_ERROR");
}

TEST(Step16ApiTest, LoginRouteMapsInvalidCreatedAtToPersistenceError) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  const auto salt = generate_salt();
  const auto password_hash = hash_password("secret1", salt);
  connection->query_hook = [salt, password_hash](const std::string& sql, const std::vector<std::string>&) {
    if (sql.find("SELECT user_id, username, role") != std::string::npos) {
      return std::optional<QueryResult>{QueryResult{.rows = {{"7", "alice", "1"}}}};
    }
    return std::optional<QueryResult>{QueryResult{
      .rows = {{"7", "alice", password_hash, salt, "1", "alice@example.com", "", "invalid-created-at"}},
    }};
  };
  auto auth = create_auth_service(*pool, "test-secret");
  CapturingHttpServer server;
  register_auth_routes(server, *pool, *auth);

  HttpRequest request;
  request.body = R"({"username":"alice","password":"secret1"})";
  HttpResponse response;
  server.post_handlers.at("/api/auth/login")(request, response);

  EXPECT_EQ(response.status_code, 500);
  EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "PERSISTENCE_ERROR");
}

TEST(Step16ApiTest, RegistrationRejectsEmailLongerThan128CharactersBeforeWriting) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  auto auth = create_auth_service(*pool, "test-secret");
  CapturingHttpServer server;
  register_auth_routes(server, *pool, *auth);
  HttpRequest request;
  request.body =
    nlohmann::json{{"username", "alice"}, {"password", "secret1"}, {"email", std::string(117, 'a') + "@example.com"}}
      .dump();
  HttpResponse response;

  server.post_handlers.at("/api/auth/register")(request, response);

  EXPECT_EQ(response.status_code, 400);
  EXPECT_EQ(connection->last_sql, "");
}

TEST(Step16ApiTest, RegistrationRejectsPasswordLongerThan128BytesBeforeWriting) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  auto auth = create_auth_service(*pool, "test-secret");
  CapturingHttpServer server;
  register_auth_routes(server, *pool, *auth);
  HttpRequest request;
  request.body =
    nlohmann::json{{"username", "alice"}, {"password", std::string(129, 'x')}, {"email", "alice@example.com"}}.dump();
  HttpResponse response;

  server.post_handlers.at("/api/auth/register")(request, response);

  EXPECT_EQ(response.status_code, 400);
  EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "INVALID_REQUEST");
  EXPECT_EQ(connection->last_sql, "");
}

TEST(Step16ApiTest, RegistrationMapsConcurrentEmailWinnerToConflictWithoutLeakingEmail) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  const std::string email = "race@example.com";
  bool insert_attempted = false;
  connection->execute_hook = [&insert_attempted](const std::string& sql,
                                                 const std::vector<std::string>&) -> std::optional<int64_t> {
    if (sql.starts_with("INSERT INTO users")) {
      insert_attempted = true;
      return std::nullopt;
    }
    return 0;
  };
  connection->query_hook = [&insert_attempted, &email](const std::string& sql,
                                                       const std::vector<std::string>&) -> std::optional<QueryResult> {
    QueryResult result;
    if (insert_attempted && sql.find("email = ?") != std::string::npos) {
      result.rows.push_back({"8", "winner", "hash", "salt", "1", email, "", "2026-01-01"});
    }
    return result;
  };
  auto auth = create_auth_service(*pool, "test-secret");
  CapturingHttpServer server;
  register_auth_routes(server, *pool, *auth);
  HttpRequest request;
  request.body = nlohmann::json{{"username", "alice"}, {"password", "secret1"}, {"email", email}}.dump();
  HttpResponse response;

  server.post_handlers.at("/api/auth/register")(request, response);

  EXPECT_EQ(response.status_code, 409);
  EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "EMAIL_CONFLICT");
  EXPECT_EQ(response.body.find(email), std::string::npos);
}

TEST(Step16ApiTest, RegistrationMapsConcurrentUsernameWinnerToConflict) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  bool insert_attempted = false;
  connection->execute_hook = [&insert_attempted](const std::string& sql,
                                                 const std::vector<std::string>&) -> std::optional<int64_t> {
    if (sql.starts_with("INSERT INTO users")) {
      insert_attempted = true;
      return std::nullopt;
    }
    return 0;
  };
  connection->query_hook = [&insert_attempted](const std::string& sql,
                                               const std::vector<std::string>&) -> std::optional<QueryResult> {
    QueryResult result;
    if (insert_attempted && sql.find("username = ?") != std::string::npos) {
      result.rows.push_back({"8", "alice", "hash", "salt", "1", "", "", "2026-01-01"});
    }
    return result;
  };
  auto auth = create_auth_service(*pool, "test-secret");
  CapturingHttpServer server;
  register_auth_routes(server, *pool, *auth);
  HttpRequest request;
  request.body = R"({"username":"alice","password":"secret1","email":""})";
  HttpResponse response;

  server.post_handlers.at("/api/auth/register")(request, response);

  EXPECT_EQ(response.status_code, 409);
  EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "USERNAME_CONFLICT");
}

TEST(Step16ApiTest, RegistrationPrechecksExistingEmailWithoutAttemptingInsert) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  int insert_count = 0;
  connection->execute_hook = [&insert_count](const std::string& sql,
                                             const std::vector<std::string>&) -> std::optional<int64_t> {
    if (sql.starts_with("INSERT INTO users")) {
      ++insert_count;
    }
    return 1;
  };
  connection->query_hook = [](const std::string& sql, const std::vector<std::string>&) -> std::optional<QueryResult> {
    QueryResult result;
    if (sql.find("email = ?") != std::string::npos) {
      result.rows.push_back({"7", "existing", "hash", "salt", "1", "used@example.com", "", "2026-01-01"});
    }
    return result;
  };
  auto auth = create_auth_service(*pool, "test-secret");
  CapturingHttpServer server;
  register_auth_routes(server, *pool, *auth);
  HttpRequest request;
  request.body = R"({"username":"alice","password":"secret1","email":"used@example.com"})";
  HttpResponse response;

  server.post_handlers.at("/api/auth/register")(request, response);

  EXPECT_EQ(response.status_code, 409);
  EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "EMAIL_CONFLICT");
  EXPECT_EQ(insert_count, 0);
}

TEST(Step16ApiTest, RegistrationMapsNonConflictInsertFailureToPersistenceError) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  connection->execute_result = std::nullopt;
  connection->query_result = QueryResult{};
  auto auth = create_auth_service(*pool, "test-secret");
  CapturingHttpServer server;
  register_auth_routes(server, *pool, *auth);
  HttpRequest request;
  request.body = R"({"username":"alice","password":"secret1","email":"alice@example.com"})";
  HttpResponse response;

  server.post_handlers.at("/api/auth/register")(request, response);

  EXPECT_EQ(response.status_code, 500);
  EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "PERSISTENCE_ERROR");
}

TEST(Step16ApiTest, RegistrationAndLoginRejectEmptyGeneratedTokensAsPersistenceErrors) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  const auto salt = generate_salt();
  const auto password_hash = hash_password("secret1", salt);
  connection->query_hook = [salt, password_hash](const std::string& sql,
                                                 const std::vector<std::string>&) -> std::optional<QueryResult> {
    if (sql.find("SELECT user_id, username, role") != std::string::npos) {
      return QueryResult{.rows = {{"7", "alice", "1"}}};
    }
    if (sql.find("WHERE user_id = ?") != std::string::npos) {
      return QueryResult{
        .rows = {{"7", "alice", password_hash, salt, "1", "alice@example.com", "", "2026-01-02 03:04:05.000000"}}};
    }
    return QueryResult{};
  };
  connection->execute_result = 1;
  EmptyTokenAuthService auth{AuthUser{7, "alice", UserRole::NORMAL}};
  CapturingHttpServer server;
  register_auth_routes(server, *pool, auth);

  HttpRequest registration;
  registration.body = R"({"username":"alice","password":"secret1","email":"alice@example.com"})";
  HttpResponse registration_response;
  server.post_handlers.at("/api/auth/register")(registration, registration_response);
  EXPECT_EQ(registration_response.status_code, 500);
  EXPECT_EQ(nlohmann::json::parse(registration_response.body).at("code"), "PERSISTENCE_ERROR");

  HttpRequest login;
  login.body = R"({"username":"alice","password":"secret1"})";
  HttpResponse login_response;
  server.post_handlers.at("/api/auth/login")(login, login_response);
  EXPECT_EQ(login_response.status_code, 500);
  EXPECT_EQ(nlohmann::json::parse(login_response.body).at("code"), "PERSISTENCE_ERROR");
}

TEST(Step19VipApiTest, PlansAreFixedAndAvailableOnlyToNormalOrVipUsers) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  CapturingHttpServer server;
  register_vip_routes(server, *pool, [] { return kRouteNow; });

  for (const auto role : {UserRole::NORMAL, UserRole::VIP}) {
    auto request = authenticated_request(7, role);
    HttpResponse response;
    server.get_handlers.at("/api/vip/plans")(request, response);
    ASSERT_EQ(response.status_code, 200);
    const auto body = nlohmann::json::parse(response.body);
    EXPECT_EQ(body.at("plans"),
              nlohmann::json::array({{{"duration_days", 30}, {"label", "30 天"}},
                                     {{"duration_days", 90}, {"label", "90 天"}},
                                     {{"duration_days", 365}, {"label", "365 天"}}}));
    EXPECT_EQ(response.body.find("price"), std::string::npos);
  }

  auto admin = authenticated_request(1, UserRole::ADMIN);
  HttpResponse admin_response;
  server.get_handlers.at("/api/vip/plans")(admin, admin_response);
  EXPECT_EQ(admin_response.status_code, 403);
  EXPECT_EQ(nlohmann::json::parse(admin_response.body).at("code"), "VIP_SELF_SERVICE_UNAVAILABLE");

  HttpRequest guest;
  HttpResponse guest_response;
  server.get_handlers.at("/api/vip/plans")(guest, guest_response);
  EXPECT_EQ(guest_response.status_code, 401);
  EXPECT_EQ(nlohmann::json::parse(guest_response.body).at("code"), "AUTH_REQUIRED");
}

TEST(Step19VipApiTest, MembershipUsesInjectedNowAndNeverReturnsNegativeRemainingSeconds) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  connection->query_result = QueryResult{.rows = {{"7",
                                                   "expired-user",
                                                   "hash",
                                                   "salt",
                                                   "2",
                                                   "quote\"slash\\@example.com",
                                                   "2033-05-18 03:33:19.000000",
                                                   "2026-01-02 03:04:05.000000"}}};
  CapturingHttpServer server;
  register_vip_routes(server, *pool, [] { return kRouteNow; });
  auto request = authenticated_request(7, UserRole::NORMAL);
  HttpResponse response;

  server.get_handlers.at("/api/vip/membership")(request, response);

  ASSERT_EQ(response.status_code, 200);
  const auto body = nlohmann::json::parse(response.body);
  EXPECT_EQ(body.at("role"), "NORMAL");
  EXPECT_EQ(body.at("vip_status"), "EXPIRED");
  EXPECT_EQ(body.at("vip_expires_at"), "2033-05-18T03:33:19.000000Z");
  EXPECT_EQ(body.at("server_now"), "2033-05-18T03:33:20.000000Z");
  EXPECT_EQ(body.at("remaining_seconds"), 0);
}

TEST(Step19VipApiTest, MembershipReturnsNoneForNormalUserWithoutExpiry) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  connection->query_result =
    QueryResult{.rows = {{"7", "normal-user", "hash", "salt", "1", "", "", "2026-01-02 03:04:05.000000"}}};
  CapturingHttpServer server;
  register_vip_routes(server, *pool, [] { return kRouteNow; });
  auto request = authenticated_request(7, UserRole::NORMAL);
  HttpResponse response;

  server.get_handlers.at("/api/vip/membership")(request, response);

  ASSERT_EQ(response.status_code, 200);
  const auto body = nlohmann::json::parse(response.body);
  EXPECT_EQ(body.at("role"), "NORMAL");
  EXPECT_EQ(body.at("vip_status"), "NONE");
  EXPECT_TRUE(body.at("vip_expires_at").is_null());
  EXPECT_EQ(body.at("remaining_seconds"), 0);
}

TEST(Step19VipApiTest, MembershipKeepsPositiveRemainingTimeAcrossClockEndpoints) {
  const auto earliest_text =
    mysql_datetime_text(format_mysql_utc_datetime(std::chrono::system_clock::time_point::min()));
  const auto latest_text = mysql_datetime_text(format_mysql_utc_datetime(std::chrono::system_clock::time_point::max()));
  const auto earliest = parse_mysql_utc_datetime(earliest_text);
  const auto latest = parse_mysql_utc_datetime(latest_text);
  if (!earliest || !latest || *latest <= *earliest) {
    GTEST_SKIP() << "本机 system_clock 两端不能同时作为 MySQL DATETIME 表示";
  }

  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  connection->query_result = QueryResult{
    .rows = {{"7", "long-lived", "hash", "salt", "2", "vip@example.com", latest_text, "2026-01-02 03:04:05.000000"}}};
  CapturingHttpServer server;
  register_vip_routes(server, *pool, [earliest] { return *earliest; });
  HttpResponse response;
  server.get_handlers.at("/api/vip/membership")(authenticated_request(7, UserRole::NORMAL), response);

  ASSERT_EQ(response.status_code, 200);
  const auto remaining = nlohmann::json::parse(response.body).at("remaining_seconds").get<int64_t>();
  EXPECT_GT(remaining, 0);
  EXPECT_LE(remaining, std::numeric_limits<int64_t>::max());
}

TEST(Step19VipApiTest, ExpiryOverflowKeepsTheExistingPublicStateErrorCode) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  connection->query_result =
    QueryResult{.rows = {{"7", "normal", "hash", "salt", "1", "", "", "2026-01-02 03:04:05.000000"}}};
  connection->execute_result = 1;
  CapturingHttpServer server;
  const auto now = std::chrono::system_clock::time_point::max() - std::chrono::hours{24};
  register_vip_routes(server, *pool, [now] { return now; });
  auto request = authenticated_request(7, UserRole::NORMAL);
  request.body = R"({"duration_days":30})";
  HttpResponse response;

  server.post_handlers.at("/api/vip/membership/activate")(request, response);

  EXPECT_EQ(response.status_code, 422);
  EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "VIP_STATE_INVALID");
}

TEST(Step19VipApiTest, MembershipAndActivateRejectPersistedGuestState) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  connection->query_result =
    QueryResult{.rows = {{"7", "guest", "hash", "salt", "0", "", "", "2026-01-02 03:04:05.000000"}}};
  connection->execute_result = 0;
  CapturingHttpServer server;
  register_vip_routes(server, *pool, [] { return kRouteNow; });
  auto request = authenticated_request(7, UserRole::NORMAL);

  HttpResponse membership;
  server.get_handlers.at("/api/vip/membership")(request, membership);
  ASSERT_EQ(membership.status_code, 422);
  EXPECT_EQ(nlohmann::json::parse(membership.body).at("code"), "VIP_STATE_INVALID");

  request.body = R"({"duration_days":30})";
  HttpResponse activation;
  server.post_handlers.at("/api/vip/membership/activate")(request, activation);
  EXPECT_EQ(activation.status_code, 422);
  EXPECT_EQ(nlohmann::json::parse(activation.body).at("code"), "VIP_STATE_INVALID");
}

TEST(Step19VipApiTest, EverySelfServiceEndpointUsesSameAuthenticationMatrix) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  CapturingHttpServer server;
  register_vip_routes(server, *pool, [] { return kRouteNow; });

  const auto invoke_all = [&server](const HttpRequest& base_request) {
    std::vector<HttpResponse> responses(3);
    server.get_handlers.at("/api/vip/plans")(base_request, responses[0]);
    server.get_handlers.at("/api/vip/membership")(base_request, responses[1]);
    auto activation_request = base_request;
    activation_request.body = R"({"duration_days":30})";
    server.post_handlers.at("/api/vip/membership/activate")(activation_request, responses[2]);
    return responses;
  };

  HttpRequest guest;
  for (const auto& response : invoke_all(guest)) {
    EXPECT_EQ(response.status_code, 401);
    EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "AUTH_REQUIRED");
  }

  const auto admin = authenticated_request(1, UserRole::ADMIN);
  for (const auto& response : invoke_all(admin)) {
    EXPECT_EQ(response.status_code, 403);
    EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "VIP_SELF_SERVICE_UNAVAILABLE");
  }

  HttpRequest storage_error;
  storage_error.auth_status = TokenValidationStatus::STORAGE_ERROR;
  for (const auto& response : invoke_all(storage_error)) {
    EXPECT_EQ(response.status_code, 500);
    EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), "PERSISTENCE_ERROR");
  }
}

TEST(Step19VipApiTest, ActivateUsesSameEndpointForFirstActivationAndRenewal) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  std::string expires;
  connection->query_hook = [&expires](const std::string& sql,
                                      const std::vector<std::string>&) -> std::optional<QueryResult> {
    if (sql.find("FOR UPDATE") != std::string::npos) {
      return QueryResult{.rows = {{"7",
                                   "vip-user",
                                   "hash",
                                   "salt",
                                   expires.empty() ? "1" : "2",
                                   "vip@example.com",
                                   expires,
                                   "2026-01-02 03:04:05.000000"}}};
    }
    return QueryResult{};
  };
  connection->execute_hook = [&expires](const std::string& sql,
                                        const std::vector<std::string>& params) -> std::optional<int64_t> {
    if (sql.starts_with("UPDATE users SET role")) {
      expires = params.at(0);
      return 1;
    }
    return 0;
  };
  CapturingHttpServer server;
  register_vip_routes(server, *pool, [] { return kRouteNow; });
  auto request = authenticated_request(7, UserRole::NORMAL);

  request.body = R"({"duration_days":30})";
  HttpResponse first;
  server.post_handlers.at("/api/vip/membership/activate")(request, first);
  ASSERT_EQ(first.status_code, 200);
  EXPECT_EQ(nlohmann::json::parse(first.body).at("remaining_seconds"), 30 * 24 * 60 * 60);

  request.body = R"({"duration_days":90})";
  HttpResponse renewal;
  server.post_handlers.at("/api/vip/membership/activate")(request, renewal);
  ASSERT_EQ(renewal.status_code, 200);
  EXPECT_EQ(nlohmann::json::parse(renewal.body).at("remaining_seconds"), 120 * 24 * 60 * 60);
}

TEST(Step19VipApiTest, ActivateRejectsInvalidJsonAndStrictDurationsWithoutThrowing) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  CapturingHttpServer server;
  register_vip_routes(server, *pool, [] { return kRouteNow; });
  auto request = authenticated_request(7, UserRole::NORMAL);

  for (const std::string body : {R"({"duration_days":31})",
                                 R"({"duration_days":30.0})",
                                 R"({"duration_days":"30"})",
                                 R"({"duration_days":true})",
                                 "{"}) {
    request.body = body;
    HttpResponse response;
    EXPECT_NO_THROW(server.post_handlers.at("/api/vip/membership/activate")(request, response));
    EXPECT_EQ(response.status_code, 400);
    const auto payload = nlohmann::json::parse(response.body);
    EXPECT_TRUE(payload.at("code") == "INVALID_VIP_DURATION" || payload.at("code") == "INVALID_REQUEST");
  }
}

TEST(Step19VipApiTest, ActivateAndAdminGrantUseExactStrictJsonContract) {
  const std::array invalid_bodies = {
    std::pair{std::string_view{R"({"duration_days":30,"duration_days":90})"}, std::string_view{"INVALID_REQUEST"}},
    std::pair{std::string_view{R"({"duration_days":30,"unexpected":true})"}, std::string_view{"INVALID_REQUEST"}},
    std::pair{std::string_view{R"({"duration_days":"30"})"}, std::string_view{"INVALID_REQUEST"}},
    std::pair{std::string_view{R"({"duration_days":30.0})"}, std::string_view{"INVALID_REQUEST"}},
    std::pair{std::string_view{R"({"duration_days":true})"}, std::string_view{"INVALID_REQUEST"}},
    std::pair{std::string_view{R"({})"}, std::string_view{"INVALID_REQUEST"}},
    std::pair{std::string_view{"{"}, std::string_view{"INVALID_REQUEST"}},
    std::pair{std::string_view{R"({"duration_days":31})"}, std::string_view{"INVALID_VIP_DURATION"}},
    std::pair{std::string_view{R"({"duration_days":-30})"}, std::string_view{"INVALID_VIP_DURATION"}},
  };
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  int mutation_count = 0;
  connection->execute_hook = [&mutation_count](const std::string& sql,
                                               const std::vector<std::string>&) -> std::optional<int64_t> {
    if (sql == "START TRANSACTION" || sql.starts_with("UPDATE users")) {
      ++mutation_count;
    }
    return 0;
  };
  CapturingHttpServer server;
  register_vip_routes(server, *pool, [] { return kRouteNow; });
  register_admin_routes(server, *pool, [] { return kRouteNow; });

  const auto assert_contract = [&invalid_bodies](const auto& handler, HttpRequest request) {
    for (const auto& [body, expected_code] : invalid_bodies) {
      request.body = body;
      HttpResponse response;
      EXPECT_NO_THROW(handler(request, response));
      ASSERT_EQ(response.status_code, 400) << body;
      EXPECT_EQ(nlohmann::json::parse(response.body).at("code"), expected_code) << body;
    }
  };

  assert_contract(server.post_handlers.at("/api/vip/membership/activate"), authenticated_request(7, UserRole::NORMAL));
  auto admin_request = authenticated_request(1, UserRole::ADMIN);
  admin_request.path_params["id"] = "7";
  assert_contract(server.post_handlers.at("/api/admin/users/:id/vip"), std::move(admin_request));
  EXPECT_EQ(mutation_count, 0);
}

TEST(Step19AdminApiTest, SearchDecodesQueryPaginatesAndReturnsCompleteEffectiveRows) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  connection->execute_result = 0;
  std::vector<std::vector<std::string>> observed_params;
  connection->query_hook = [&observed_params](const std::string& sql,
                                              const std::vector<std::string>& params) -> std::optional<QueryResult> {
    observed_params.push_back(params);
    if (sql.starts_with("SELECT COUNT")) {
      return QueryResult{.rows = {{"1"}}};
    }
    return QueryResult{.rows = {{"9",
                                 "特殊\"用户",
                                 "hash",
                                 "salt",
                                 "2",
                                 "mail+tag@example.com",
                                 "2033-05-18 03:33:19.000000",
                                 "2026-01-02 03:04:05.000000"}}};
  };
  CapturingHttpServer server;
  register_admin_routes(server, *pool, [] { return kRouteNow; });
  auto request = authenticated_request(1, UserRole::ADMIN);
  request.query_string = "q=mail%2Btag%40example.com&offset=2&limit=5";
  HttpResponse response;

  server.get_handlers.at("/api/admin/users")(request, response);

  ASSERT_EQ(response.status_code, 200);
  const auto body = nlohmann::json::parse(response.body);
  EXPECT_EQ(body.at("total"), 1);
  EXPECT_EQ(body.at("offset"), 2);
  EXPECT_EQ(body.at("limit"), 5);
  ASSERT_EQ(body.at("items").size(), 1);
  const auto& item = body.at("items").at(0);
  EXPECT_EQ(item.at("user_id"), 9);
  EXPECT_EQ(item.at("username"), "特殊\"用户");
  EXPECT_EQ(item.at("email"), "mail+tag@example.com");
  EXPECT_EQ(item.at("role"), "NORMAL");
  EXPECT_EQ(item.at("vip_status"), "EXPIRED");
  EXPECT_EQ(item.at("vip_expires_at"), "2033-05-18T03:33:19.000000Z");
  EXPECT_EQ(item.at("created_at"), "2026-01-02T03:04:05.000000Z");
  EXPECT_FALSE(item.contains("capabilities"));
  ASSERT_GE(observed_params.size(), 2U);
  EXPECT_EQ(observed_params[0].at(0), "%mail+tag@example.com%");
  EXPECT_EQ(observed_params[1].at(0), "%mail+tag@example.com%");
}

TEST(Step19AdminApiTest, UserListJsonEscapesBackslashesNewlinesAndControlCharacters) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  connection->execute_result = 0;
  std::string username = "user\\folder\nname";
  username.push_back('\x01');
  std::string email = "mail\\box\n@example.com";
  email.push_back('\x01');
  connection->query_hook = [username, email](const std::string& sql,
                                             const std::vector<std::string>&) -> std::optional<QueryResult> {
    if (sql.starts_with("SELECT COUNT")) {
      return QueryResult{.rows = {{"1"}}};
    }
    return QueryResult{.rows = {{"9", username, "hash", "salt", "1", email, "", "2026-01-02 03:04:05.000000"}}};
  };
  CapturingHttpServer server;
  register_admin_routes(server, *pool, [] { return kRouteNow; });
  const auto request = authenticated_request(1, UserRole::ADMIN);
  HttpResponse response;

  server.get_handlers.at("/api/admin/users")(request, response);

  ASSERT_EQ(response.status_code, 200);
  const auto body = nlohmann::json::parse(response.body);
  ASSERT_EQ(body.at("items").size(), 1);
  EXPECT_EQ(body.at("items").at(0).at("username"), username);
  EXPECT_EQ(body.at("items").at(0).at("email"), email);
}

TEST(Step19AdminApiTest, VipGrantRenewAndRevokeReturnCompleteRows) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  std::string expires;
  connection->query_hook = [&expires](const std::string& sql,
                                      const std::vector<std::string>&) -> std::optional<QueryResult> {
    if (sql.find("FOR UPDATE") != std::string::npos) {
      return QueryResult{.rows = {{"9",
                                   "candidate",
                                   "hash",
                                   "salt",
                                   expires.empty() ? "1" : "2",
                                   "candidate@example.com",
                                   expires,
                                   "2026-01-02 03:04:05.000000"}}};
    }
    return QueryResult{};
  };
  connection->execute_hook = [&expires](const std::string& sql,
                                        const std::vector<std::string>& params) -> std::optional<int64_t> {
    if (sql.starts_with("UPDATE users SET role = 2")) {
      expires = params.at(0);
      return 1;
    }
    if (sql.starts_with("UPDATE users SET role = 1")) {
      expires.clear();
      return 1;
    }
    return 0;
  };
  CapturingHttpServer server;
  register_admin_routes(server, *pool, [] { return kRouteNow; });
  auto request = authenticated_request(1, UserRole::ADMIN);
  request.path_params["id"] = "9";

  for (const int days : {30, 90}) {
    request.body = nlohmann::json{{"duration_days", days}}.dump();
    HttpResponse response;
    server.post_handlers.at("/api/admin/users/:id/vip")(request, response);
    ASSERT_EQ(response.status_code, 200);
    const auto row = nlohmann::json::parse(response.body);
    EXPECT_EQ(row.at("user_id"), 9);
    EXPECT_EQ(row.at("username"), "candidate");
    EXPECT_EQ(row.at("email"), "candidate@example.com");
    EXPECT_EQ(row.at("role"), "VIP");
    EXPECT_EQ(row.at("vip_status"), "ACTIVE");
    EXPECT_TRUE(row.contains("created_at"));
  }

  HttpResponse revoked;
  server.del_handlers.at("/api/admin/users/:id/vip")(request, revoked);
  ASSERT_EQ(revoked.status_code, 200);
  const auto row = nlohmann::json::parse(revoked.body);
  EXPECT_EQ(row.at("role"), "NORMAL");
  EXPECT_EQ(row.at("vip_status"), "NONE");
  EXPECT_TRUE(row.at("vip_expires_at").is_null());
}

TEST(Step19AdminApiTest, AuthorizationAdminTargetAndInvalidInputsUseStableErrors) {
  MockConnection* connection = nullptr;
  auto pool = make_database_pool(connection);
  ASSERT_NE(connection, nullptr);
  connection->execute_result = 0;
  connection->query_result = QueryResult{
    .rows = {{"1", "admin", "hash", "salt", "3", "admin@example.invalid", "", "2026-01-02 03:04:05.000000"}}};
  CapturingHttpServer server;
  register_admin_routes(server, *pool, [] { return kRouteNow; });

  auto normal = authenticated_request(7, UserRole::NORMAL);
  HttpResponse forbidden;
  server.get_handlers.at("/api/admin/users")(normal, forbidden);
  EXPECT_EQ(forbidden.status_code, 403);
  EXPECT_EQ(nlohmann::json::parse(forbidden.body).at("code"), "ADMIN_REQUIRED");

  auto admin = authenticated_request(1, UserRole::ADMIN);
  admin.path_params["id"] = "1";
  admin.body = R"({"duration_days":30})";
  HttpResponse conflict;
  server.post_handlers.at("/api/admin/users/:id/vip")(admin, conflict);
  EXPECT_EQ(conflict.status_code, 409);
  EXPECT_EQ(nlohmann::json::parse(conflict.body).at("code"), "ADMIN_MEMBERSHIP_FORBIDDEN");

  for (const std::string query : {"offset=-1", "limit=0", "limit=101", "offset=x", "q=%ZZ"}) {
    admin.query_string = query;
    HttpResponse invalid_query;
    EXPECT_NO_THROW(server.get_handlers.at("/api/admin/users")(admin, invalid_query));
    EXPECT_EQ(invalid_query.status_code, 400);
    EXPECT_EQ(nlohmann::json::parse(invalid_query.body).at("code"), "INVALID_REQUEST");
  }

  admin.path_params["id"] = "1x";
  HttpResponse invalid_path;
  EXPECT_NO_THROW(server.del_handlers.at("/api/admin/users/:id/vip")(admin, invalid_path));
  EXPECT_EQ(invalid_path.status_code, 400);
  EXPECT_EQ(nlohmann::json::parse(invalid_path.body).at("code"), "INVALID_REQUEST");
}

} // namespace hps

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
