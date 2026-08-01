#pragma once

#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string_view>

namespace hps {

class IDatabasePool;
class IHttpServer;

void register_playlist_routes(IHttpServer& server, IDatabasePool& database);
std::optional<nlohmann::json> parse_playlist_json_object(std::string_view body);

} // namespace hps
