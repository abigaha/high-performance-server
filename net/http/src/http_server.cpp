#include "http_server.h"

#include "authorization.h"
#include "file_system.h"
#include "logger.h"
#include "thread_pool.h"
#include "websocket.h"
#include "ws_connection.h"

#include <openssl/evp.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hps {

namespace {

HttpResponse upload_auth_error(TokenValidationStatus status) {
  HttpResponse response;
  const bool storage_error = status == TokenValidationStatus::STORAGE_ERROR;
  response.set_status(storage_error ? 500 : 401, storage_error ? "Internal Server Error" : "Unauthorized");
  response.set_content_type("application/json");
  response.body = nlohmann::json{{"code", storage_error ? "PERSISTENCE_ERROR" : "AUTH_REQUIRED"},
                                 {"error", storage_error ? "认证存储暂时不可用" : "需要登录"}}
                    .dump();
  response.set_content_length(response.body.size());
  return response;
}

void add_cors_headers(HttpResponse& resp) {
  resp.set_header("Access-Control-Allow-Origin", "*");
  resp.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
  resp.set_header("Access-Control-Allow-Headers", "Authorization, Content-Type, Content-Disposition, Range");
  resp.set_header("Access-Control-Expose-Headers", "Content-Range, Accept-Ranges, Content-Disposition");
  resp.set_header("Access-Control-Max-Age", "86400");
}

std::string_view trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return value;
}

bool ascii_iequals(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    const auto lhs_value = static_cast<unsigned char>(lhs[index]);
    const auto rhs_value = static_cast<unsigned char>(rhs[index]);
    if (std::tolower(lhs_value) != std::tolower(rhs_value)) {
      return false;
    }
  }
  return true;
}

std::optional<unsigned int> hex_value(char value) {
  if (value >= '0' && value <= '9') {
    return static_cast<unsigned int>(value - '0');
  }
  if (value >= 'a' && value <= 'f') {
    return static_cast<unsigned int>(value - 'a' + 10);
  }
  if (value >= 'A' && value <= 'F') {
    return static_cast<unsigned int>(value - 'A' + 10);
  }
  return std::nullopt;
}

bool valid_utf8(std::string_view value) {
  std::size_t index = 0;
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first <= 0x7FU) {
      ++index;
      continue;
    }

    std::size_t continuation_count = 0;
    uint32_t code_point = 0;
    if (first >= 0xC2U && first <= 0xDFU) {
      continuation_count = 1;
      code_point = first & 0x1FU;
    } else if (first >= 0xE0U && first <= 0xEFU) {
      continuation_count = 2;
      code_point = first & 0x0FU;
    } else if (first >= 0xF0U && first <= 0xF4U) {
      continuation_count = 3;
      code_point = first & 0x07U;
    } else {
      return false;
    }

    if (index + continuation_count >= value.size()) {
      return false;
    }
    for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
      const auto next = static_cast<unsigned char>(value[index + offset]);
      if ((next & 0xC0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6U) | (next & 0x3FU);
    }

    if ((continuation_count == 2 && code_point < 0x800U) || (continuation_count == 3 && code_point < 0x10000U) ||
        (code_point >= 0xD800U && code_point <= 0xDFFFU) || code_point > 0x10FFFFU) {
      return false;
    }
    index += continuation_count + 1;
  }
  return true;
}

std::optional<std::string> safe_file_name(std::string value) {
  const auto separator = value.find_last_of("/\\");
  if (separator != std::string::npos) {
    value.erase(0, separator + 1);
  }
  if (value.empty() || value == "." || value == ".." || !valid_utf8(value)) {
    return std::nullopt;
  }
  const bool contains_control =
    std::ranges::any_of(value, [](unsigned char byte) { return byte < 0x20U || byte == 0x7FU; });
  if (contains_control) {
    return std::nullopt;
  }
  return value;
}

std::optional<std::string> decode_extended_filename(std::string_view value) {
  const auto first_quote = value.find('\'');
  if (first_quote == std::string_view::npos) {
    return std::nullopt;
  }
  const auto second_quote = value.find('\'', first_quote + 1);
  if (second_quote == std::string_view::npos || !ascii_iequals(value.substr(0, first_quote), "UTF-8")) {
    return std::nullopt;
  }

  std::string decoded;
  const auto encoded = value.substr(second_quote + 1);
  decoded.reserve(encoded.size());
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    if (encoded[index] != '%') {
      decoded.push_back(encoded[index]);
      continue;
    }
    if (index + 2 >= encoded.size()) {
      return std::nullopt;
    }
    const auto high = hex_value(encoded[index + 1]);
    const auto low = hex_value(encoded[index + 2]);
    if (!high || !low) {
      return std::nullopt;
    }
    decoded.push_back(static_cast<char>((*high << 4U) | *low));
    index += 2;
  }
  return safe_file_name(std::move(decoded));
}

struct FilenameCandidates {
  std::optional<std::string> regular;
  std::optional<std::string> extended;
};

std::vector<std::string_view> split_disposition_parameters(std::string_view header_value) {
  std::vector<std::string_view> parameters;
  std::size_t segment_start = 0;
  bool quoted = false;
  bool escaped = false;
  for (std::size_t index = 0; index < header_value.size(); ++index) {
    const char current = header_value[index];
    if (escaped) {
      escaped = false;
    } else if (quoted && current == '\\') {
      escaped = true;
    } else if (current == '"') {
      quoted = !quoted;
    } else if (!quoted && current == ';') {
      parameters.push_back(header_value.substr(segment_start, index - segment_start));
      segment_start = index + 1;
    }
  }
  parameters.push_back(header_value.substr(segment_start));
  return parameters;
}

std::optional<std::string> decode_parameter_value(std::string_view value) {
  value = trim(value);
  if (value.empty() || value.front() != '"') {
    return std::string(value);
  }

  std::string decoded;
  bool escaped = false;
  for (std::size_t index = 1; index < value.size(); ++index) {
    const char current = value[index];
    if (escaped) {
      decoded.push_back(current);
      escaped = false;
    } else if (current == '\\') {
      escaped = true;
    } else if (current == '"') {
      if (!trim(value.substr(index + 1)).empty()) {
        return std::nullopt;
      }
      return decoded;
    } else {
      decoded.push_back(current);
    }
  }
  return std::nullopt;
}

FilenameCandidates parse_filename_candidates(std::string_view header_value) {
  FilenameCandidates candidates;
  const auto parameters = split_disposition_parameters(header_value);
  for (std::size_t index = 1; index < parameters.size(); ++index) {
    const auto equals = parameters[index].find('=');
    if (equals == std::string_view::npos) {
      continue;
    }
    const auto name = trim(parameters[index].substr(0, equals));
    auto value = decode_parameter_value(parameters[index].substr(equals + 1));
    if (!value) {
      continue;
    }
    if (ascii_iequals(name, "filename*") && !candidates.extended) {
      candidates.extended = decode_extended_filename(*value);
    } else if (ascii_iequals(name, "filename") && !candidates.regular) {
      candidates.regular = safe_file_name(std::move(*value));
    }
  }
  return candidates;
}

void enable_discard_mode(HttpParser& parser) {
  parser.set_streaming_mode(true);
  parser.set_chunk_handler([](std::string_view) { return true; });
}

void append_response(Connection& conn, HttpResponse response) {
  add_cors_headers(response);
  auto serialized = response.serialize();
  std::lock_guard<std::mutex> lock(conn.write_mutex());
  conn.write_buffer().append(serialized);
}

void parse_upload_metadata(const HttpRequest& request, UploadStreamContext& context, bool use_default_name) {
  const auto disposition = request.headers.find("Content-Disposition");
  if (disposition != request.headers.end()) {
    const auto file_name = parse_content_disposition_filename(disposition->second);
    if (file_name) {
      context.file_name = *file_name;
    }
  }
  if (context.file_name.empty() && use_default_name) {
    context.file_name = "unnamed";
  }

  const auto content_length = request.headers.find("Content-Length");
  if (content_length == request.headers.end()) {
    return;
  }
  uint64_t parsed_length = 0;
  const auto* begin = content_length->second.data();
  const auto* end = begin + content_length->second.size();
  const auto result = std::from_chars(begin, end, parsed_length);
  if (result.ec == std::errc{} && result.ptr == end &&
      parsed_length <= static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
    context.content_length = static_cast<std::size_t>(parsed_length);
    context.total_size = *context.content_length;
  }
}

bool initialize_upload_hash(UploadStreamContext& context) {
  auto* md_context = EVP_MD_CTX_new();
  if (md_context == nullptr || EVP_DigestInit_ex(md_context, EVP_sha256(), nullptr) != 1) {
    EVP_MD_CTX_free(md_context);
    return false;
  }
  context.hash_ctx = md_context;
  return true;
}

bool process_upload_data(UploadStreamContext& context, std::string_view chunk) {
  if (context.hash_ctx == nullptr && !initialize_upload_hash(context)) {
    context.failed = true;
    return false;
  }

  auto* hash_context = static_cast<EVP_MD_CTX*>(context.hash_ctx);
  if (EVP_DigestUpdate(hash_context, chunk.data(), chunk.size()) != 1) {
    context.failed = true;
    return false;
  }

  auto chunk_hash = FileSystem::sha256_hex(chunk.data(), chunk.size());
  if (context.store_chunk_data && !context.store_chunk_data(chunk, chunk_hash)) {
    context.failed = true;
    return false;
  }

  FileChunkRecord record;
  record.chunk_index = static_cast<int>(context.chunks.size());
  record.chunk_hash = std::move(chunk_hash);
  record.chunk_offset = context.current_offset;
  record.chunk_size = static_cast<int>(chunk.size());
  context.current_offset += chunk.size();
  if (!context.content_length) {
    context.total_size = context.current_offset;
  }
  context.chunks.push_back(std::move(record));
  return true;
}

bool finish_initial_chunk_probe(UploadStreamContext& context, HttpParser& parser) {
  if (context.initial_chunk_probe_completed || !context.initial_chunk_probe || context.rejection_response) {
    return true;
  }
  const auto rejection = context.initial_chunk_probe(context.initial_chunk_probe_data);
  if (rejection) {
    context.rejection_response = *rejection;
    context.initial_chunk_probe_completed = true;
    context.initial_chunk_probe_data.clear();
    parser.set_stream_chunk_size(HttpParser::kStreamChunkSize);
    return true;
  }
  context.initial_chunk_probe_completed = true;
  parser.set_stream_chunk_size(HttpParser::kStreamChunkSize);
  auto probe_data = std::move(context.initial_chunk_probe_data);
  context.initial_chunk_probe_data.clear();
  return process_upload_data(context, probe_data);
}

bool process_upload_chunk(UploadStreamContext& context, HttpParser& parser, std::string_view chunk) {
  if (context.rejection_response) {
    return true;
  }
  if (context.initial_chunk_probe && !context.initial_chunk_probe_completed) {
    context.initial_chunk_probe_data.append(chunk);
    if (context.initial_chunk_probe_data.size() < context.initial_chunk_probe_size) {
      return true;
    }
    return finish_initial_chunk_probe(context, parser);
  }
  return process_upload_data(context, chunk);
}

bool finish_upload_stream(UploadStreamContext& context, HttpParser& parser) {
  return finish_initial_chunk_probe(context, parser);
}

void enable_upload_streaming(HttpParser& parser, const std::shared_ptr<UploadStreamContext>& context) {
  parser.set_streaming_mode(true);
  if (context->initial_chunk_probe && context->initial_chunk_probe_size > 0) {
    parser.set_stream_chunk_size(context->initial_chunk_probe_size);
  }
  parser.set_chunk_handler(
    [context, &parser](std::string_view chunk) { return process_upload_chunk(*context, parser, chunk); });
  parser.set_body_done_callback([context, &parser] { return finish_upload_stream(*context, parser); });
}

} // namespace

UploadStreamContext::~UploadStreamContext() {
  reset_hash_context();
}

void UploadStreamContext::reset_hash_context() noexcept {
  EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(hash_ctx));
  hash_ctx = nullptr;
}

void UploadStreamContext::set_initial_chunk_probe(std::size_t probe_size, InitialChunkProbe probe) {
  if (!probe || probe_size == 0) {
    initial_chunk_probe = nullptr;
    initial_chunk_probe_size = 0;
    initial_chunk_probe_data.clear();
    initial_chunk_probe_completed = false;
    return;
  }
  initial_chunk_probe = std::move(probe);
  initial_chunk_probe_size = probe_size;
  initial_chunk_probe_data.clear();
  initial_chunk_probe_completed = false;
}

std::optional<std::string> parse_content_disposition_filename(std::string_view header_value) {
  const auto candidates = parse_filename_candidates(header_value);
  if (candidates.extended) {
    return candidates.extended;
  }
  return candidates.regular;
}

HttpServer::HttpServer(const TcpServer::Config& config) : server_(config) {
  server_.set_close_handler([this](Connection* conn) { cleanup_connection(conn); });
}

HttpServer::~HttpServer() {
  server_.set_close_handler(nullptr);
}

bool HttpServer::init() {
  server_.set_handler([this](std::shared_ptr<Connection> conn) {
    auto cm = get_conn_mutex(conn.get());
    std::lock_guard<std::mutex> cl(*cm);
    handle_connection(*conn);
  });
  return server_.init();
}

void HttpServer::start() {
  server_.start();
}

void HttpServer::stop() {
  server_.stop();
}

void HttpServer::get(std::string_view path, Handler handler) {
  router_.add(HttpMethod::GET, path, std::move(handler));
}

void HttpServer::post(std::string_view path, Handler handler) {
  router_.add(HttpMethod::POST, path, std::move(handler));
}

void HttpServer::put(std::string_view path, Handler handler) {
  router_.add(HttpMethod::PUT, path, std::move(handler));
}

void HttpServer::del(std::string_view path, Handler handler) {
  router_.add(HttpMethod::DELETE, path, std::move(handler));
}

void HttpServer::ws(std::string_view path, WsHandler handler) {
  ws_handlers_[std::string(path)] = std::move(handler);
}

void HttpServer::upload(std::string_view path,
                        UploadHandler handler,
                        UploadStreamSetup setup,
                        UploadPreflight preflight) {
  upload_handlers_[std::string(path)] = std::move(handler);
  if (setup) {
    upload_setups_[std::string(path)] = std::move(setup);
  }
  if (preflight) {
    upload_preflights_[std::string(path)] = std::move(preflight);
  }
  // 注册 POST 路由确保请求能通过路由器匹配，进入 upload handler 调用路径
  router_.add(HttpMethod::POST, path, [](const HttpRequest&, HttpResponse&) {});
}

void HttpServer::cleanup_connection(Connection* conn) {
  std::lock_guard lock(parsers_mutex_);
  parsers_.erase(conn);
  upload_contexts_.erase(conn);
  {
    std::lock_guard conn_lock(conn_map_mutex_);
    conn_mutexes_.erase(conn);
  }
}

std::shared_ptr<std::mutex> HttpServer::get_conn_mutex(Connection* c) {
  std::lock_guard<std::mutex> lock(conn_map_mutex_);
  auto& m = conn_mutexes_[c];
  if (!m) {
    m = std::make_shared<std::mutex>();
  }
  return m;
}

bool HttpServer::try_handle_ws_upgrade(Connection& conn, const HttpRequest& req, std::size_t total_consumed) {
  auto ws_it = ws_handlers_.find(req.path);
  if (ws_it == ws_handlers_.end()) {
    return false;
  }

  HttpResponse ws_resp;
  if (!ws_server_handshake(req, ws_resp)) {
    return false;
  }

  {
    auto serialized = ws_resp.serialize();
    std::lock_guard<std::mutex> wlock(conn.write_mutex());
    conn.write_buffer().append(serialized);
    conn.write_to_fd_locked();
  }
  conn.consume_read_buffer(total_consumed);

  auto conn_ptr = conn.shared_from_this();
  auto ws_conn = std::make_shared<WsConnection>(conn_ptr, [](WsFrame) {}, [](uint16_t) {});

  WsHandler ws_handler = ws_it->second;
  ws_handler(req, ws_conn);
  ws_conn->start_event_loop();
  return true;
}

void HttpServer::on_headers_done(HttpParser& parser, const HttpRequest& req, std::shared_ptr<UploadStreamContext> ctx) {
  const auto upload_it = upload_handlers_.find(req.path);
  if (upload_it == upload_handlers_.end()) {
    return;
  }

  parse_upload_metadata(req, *ctx, !upload_preflights_.contains(req.path));

  HttpRequest authenticated_request = req;
  if (auth_service_ != nullptr) {
    AuthMiddleware::apply(*auth_service_, authenticated_request);
    if (authenticated_request.auth_status == TokenValidationStatus::STORAGE_ERROR) {
      ctx->rejection_response = upload_auth_error(authenticated_request.auth_status);
      enable_discard_mode(parser);
      return;
    }
    if (authenticated_request.auth_status != TokenValidationStatus::AUTHENTICATED ||
        !has_capability(authenticated_request.auth_user, Capability::USE_AUTHENTICATED_FEATURES)) {
      ctx->rejection_response = upload_auth_error(authenticated_request.auth_status);
      enable_discard_mode(parser);
      return;
    }
  }

  const auto preflight_it = upload_preflights_.find(req.path);
  if (preflight_it != upload_preflights_.end()) {
    ctx->rejection_response = preflight_it->second(authenticated_request, *ctx);
    if (ctx->rejection_response) {
      enable_discard_mode(parser);
      return;
    }
  }

  const auto setup_it = upload_setups_.find(req.path);
  if (setup_it != upload_setups_.end()) {
    setup_it->second(authenticated_request, *ctx, parser);
  }

  enable_upload_streaming(parser, ctx);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void HttpServer::handle_connection(Connection& conn) {
  std::shared_ptr<HttpParser> parser_ptr;
  std::shared_ptr<UploadStreamContext> upload_ctx;
  {
    std::lock_guard lock(parsers_mutex_);
    if (conn.state() == Connection::State::CLOSED) {
      parsers_.erase(&conn);
      upload_contexts_.erase(&conn);
      return;
    }
    auto& stored_parser = parsers_[&conn];
    if (!stored_parser) {
      stored_parser = std::make_shared<HttpParser>();
    }
    parser_ptr = stored_parser;
    auto& stored_context = upload_contexts_[&conn];
    if (!stored_context) {
      stored_context = std::make_shared<UploadStreamContext>();
    }
    upload_ctx = stored_context;
  }
  auto& parser = *parser_ptr;
  std::size_t total_consumed = 0;

  const auto reset_request_state = [&]() {
    parser.reset();
    upload_ctx = std::make_shared<UploadStreamContext>();
    std::lock_guard lock(parsers_mutex_);
    if (conn.state() != Connection::State::CLOSED) {
      upload_contexts_[&conn] = upload_ctx;
    }
  };
  const auto erase_connection_state = [&]() {
    std::lock_guard lock(parsers_mutex_);
    parsers_.erase(&conn);
    upload_contexts_.erase(&conn);
  };

  std::string buf;
  {
    std::lock_guard<std::mutex> rlock(conn.read_mutex());
    buf = conn.read_buffer();
  }

  while (total_consumed < buf.size()) {
    parser.set_headers_done_callback(
      [this, &parser, upload_ctx](const HttpRequest& req) { on_headers_done(parser, req, upload_ctx); });

    auto remaining = std::string_view(buf.data() + total_consumed, buf.size() - total_consumed);
    auto result = parser.feed(remaining);
    total_consumed += result.consumed;

    if (result.err == ParserError::INCOMPLETE) {
      break;
    }

    if (result.err == ParserError::BAD_REQUEST) {
      if (upload_ctx->rejection_response) {
        append_response(conn, *upload_ctx->rejection_response);
      } else if (upload_ctx->failed) {
        send_error(conn, 400, "Bad Request", "上传失败");
      } else {
        send_error(conn, 400, "Bad Request", "请求格式错误");
      }
      conn.consume_read_buffer(total_consumed);
      erase_connection_state();
      return;
    }

    if (result.err == ParserError::PAYLOAD_TOO_LARGE) {
      if (upload_ctx->rejection_response) {
        append_response(conn, *upload_ctx->rejection_response);
      } else {
        send_error(conn, 413, "Payload Too Large", "请求体过大");
      }
      conn.consume_read_buffer(total_consumed);
      erase_connection_state();
      return;
    }

    if (parser.state() != ParserState::COMPLETE) {
      break;
    }

    const auto& req = parser.request();

    if (req.method == HttpMethod::OPTIONS) {
      HttpResponse resp;
      resp.set_status(204, "No Content");
      append_response(conn, std::move(resp));
      reset_request_state();
      continue;
    }

    auto up_it = req.headers.find("Upgrade");
    if (up_it != req.headers.end() && up_it->second == "websocket") {
      bool upgraded = try_handle_ws_upgrade(conn, req, total_consumed);
      if (!upgraded) {
        send_error(conn, 400, "Bad Request", "WebSocket 握手失败");
        conn.consume_read_buffer(total_consumed);
      }
      erase_connection_state();
      return;
    }

    Router::Handler handler;
    std::unordered_map<std::string, std::string> params;
    bool matched = router_.match(req.method, req.path, handler, params);

    if (!matched) {
      if (router_.path_exists(req.path)) {
        send_error(conn, 405, "Method Not Allowed", req.path);
      } else {
        send_error(conn, 404, "Not Found", req.path);
      }
      reset_request_state();
      continue;
    }

    HttpRequest req_with_params = req;
    req_with_params.path_params = std::move(params);
    if (auth_service_ != nullptr) {
      AuthMiddleware::apply(*auth_service_, req_with_params);
    }

    HttpResponse resp;

    auto uit = upload_handlers_.find(req_with_params.path);
    if (uit != upload_handlers_.end()) {
      if (upload_ctx->rejection_response) {
        resp = *upload_ctx->rejection_response;
      } else {
        try {
          uit->second(req_with_params, *upload_ctx, resp);
        } catch (const std::exception& e) {
          Logger::_error("upload handler 异常: " + std::string(e.what()));
          send_error(conn, 500, "Internal Server Error", e.what());
          reset_request_state();
          continue;
        } catch (...) {
          Logger::_error("upload handler 未知异常");
          send_error(conn, 500, "Internal Server Error", "unknown error");
          reset_request_state();
          continue;
        }
      }
      append_response(conn, std::move(resp));
      reset_request_state();
      continue;
    }

    try {
      handler(req_with_params, resp);
    } catch (const std::exception& e) {
      Logger::_error("handler 异常: " + std::string(e.what()));
      send_error(conn, 500, "Internal Server Error", e.what());
      reset_request_state();
      continue;
    } catch (...) {
      Logger::_error("handler 未知异常");
      send_error(conn, 500, "Internal Server Error", "unknown error");
      reset_request_state();
      continue;
    }

    append_response(conn, std::move(resp));
    reset_request_state();
  }

  conn.consume_read_buffer(total_consumed);
}

void HttpServer::send_error(Connection& conn, int status, std::string_view text, std::string_view detail) {
  HttpResponse resp;
  resp.set_status(status, text);
  resp.set_content_type("text/plain; charset=utf-8");
  resp.body = std::to_string(status) + " " + std::string(text) + ": " + std::string(detail);
  resp.set_content_length(resp.body.size());
  add_cors_headers(resp);

  auto serialized = resp.serialize();
  std::lock_guard<std::mutex> wlock(conn.write_mutex());
  conn.write_buffer().append(serialized);
}

} // namespace hps
