#include "router.h"

namespace hps {

Router::Router() : root_(std::make_unique<Node>()) {}

Router::~Router() = default;

std::vector<std::string> Router::split_path(std::string_view path) {
  std::vector<std::string> segments;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      if (i > start) {
        segments.emplace_back(path.substr(start, i - start));
      }
      start = i + 1;
    }
  }
  return segments;
}

void Router::add(HttpMethod method, std::string_view path, Handler handler) {
  auto segments = split_path(path);
  insert(*root_, segments, 0, method, std::move(handler));
}

// NOLINTNEXTLINE(misc-no-recursion) trie 树结构固有递归，改迭代会显著降低可读性
void Router::insert(Node& node,
                    const std::vector<std::string>& segments,
                    std::size_t idx,
                    HttpMethod method,
                    Handler handler) {
  if (idx == segments.size()) {
    node.handlers[method] = std::move(handler);
    return;
  }

  const auto& seg = segments[idx];
  if (!seg.empty() && seg[0] == ':') {
    // 参数段
    auto param_name = seg.substr(1);
    if (!node.paramChild) {
      node.paramChild = std::make_unique<Node>();
      node.paramName = param_name;
    }
    insert(*node.paramChild, segments, idx + 1, method, std::move(handler));
  } else {
    // 静态段
    auto it = node.staticChildren.find(seg);
    if (it == node.staticChildren.end()) {
      auto [inserted, ok] = node.staticChildren.emplace(seg, std::make_unique<Node>());
      it = inserted;
      (void)ok;
    }
    insert(*it->second, segments, idx + 1, method, std::move(handler));
  }
}

bool Router::match(HttpMethod method,
                   std::string_view path,
                   Handler& outHandler,
                   std::unordered_map<std::string, std::string>& outParams) const {
  auto segments = split_path(path);
  return search(*root_, segments, 0, method, outHandler, outParams);
}

// NOLINTNEXTLINE(misc-no-recursion) trie 树结构固有递归，回溯匹配需递归
bool Router::search(const Node& node,
                    const std::vector<std::string>& segments,
                    std::size_t idx,
                    HttpMethod method,
                    Handler& outHandler,
                    std::unordered_map<std::string, std::string>& outParams) const {
  if (idx == segments.size()) {
    auto it = node.handlers.find(method);
    if (it != node.handlers.end()) {
      outHandler = it->second;
      return true;
    }
    return false;
  }

  const auto& seg = segments[idx];

  // 静态段优先
  auto it = node.staticChildren.find(seg);
  if (it != node.staticChildren.end()) {
    auto saved = outParams;
    if (search(*it->second, segments, idx + 1, method, outHandler, outParams)) {
      return true;
    }
    outParams = std::move(saved); // 回溯
  }

  // 参数段兜底
  if (node.paramChild) {
    auto saved = outParams;
    outParams[node.paramName] = seg;
    if (search(*node.paramChild, segments, idx + 1, method, outHandler, outParams)) {
      return true;
    }
    outParams = std::move(saved); // 回溯
  }

  return false;
}

bool Router::path_exists(std::string_view path) const {
  auto segments = split_path(path);
  Handler dummy_handler;
  Params dummy_params;
  return search(*root_, segments, 0, HttpMethod::GET, dummy_handler, dummy_params);
}

} // namespace hps
