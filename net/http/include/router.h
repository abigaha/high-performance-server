#pragma once

#include "http_request.h"
#include "http_response.h"
#include "i_router.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hps {

class Router : public IRouter {
public:
  using Handler = IRouter::Handler;
  using Params = IRouter::Params;

  Router();
  ~Router() override;

  Router(const Router&) = delete;
  Router& operator=(const Router&) = delete;

  void add(HttpMethod method, std::string_view path, Handler handler) override;
  bool match(HttpMethod method, std::string_view path, Handler& outHandler, Params& outParams) const override;

private:
  struct Node {
    std::unordered_map<std::string, std::unique_ptr<Node>> staticChildren;
    std::unique_ptr<Node> paramChild;
    std::string paramName;
    std::unordered_map<HttpMethod, Handler> handlers;
  };

  std::unique_ptr<Node> root_;

  static std::vector<std::string> split_path(std::string_view path);
  void insert(Node& node,
              const std::vector<std::string>& segments,
              std::size_t idx,
              HttpMethod method,
              Handler handler);
  bool search(const Node& node,
              const std::vector<std::string>& segments,
              std::size_t idx,
              HttpMethod method,
              Handler& outHandler,
              Params& outParams) const;
};

} // namespace hps
