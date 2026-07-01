#pragma once

#include "http_request.h"
#include "http_response.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hps {

/**
 * HTTP 路由器
 *
 * 基于前缀树（trie）的路由匹配，支持静态段与参数段（:name）。
 * 静态段优先于参数段匹配。
 *
 * 路径归一化：前导/尾随斜杠与连续斜杠在分割时忽略空段，
 * 即 "/api/health" 与 "/api/health/" 视为同一路由。
 */
class Router {
public:
  /** 路由处理器签名 */
  using Handler = std::function<void(const HttpRequest&, HttpResponse&)>;

  Router();
  ~Router();
  Router(const Router&) = delete;
  Router& operator=(const Router&) = delete;

  /**
   * 注册路由
   * @param method HTTP 方法
   * @param path 路径（支持 :param 参数段，如 "/song/:id"）
   * @param handler 处理器
   */
  void add(HttpMethod method, std::string_view path, Handler handler);

  /**
   * 匹配路由
   * @param method 请求方法
   * @param path 请求路径
   * @param outHandler 输出匹配的 handler（未匹配时不变）
   * @param outParams 输出路径参数
   * @return true 匹配成功；false 无匹配
   */
  bool match(HttpMethod method,
             std::string_view path,
             Handler& outHandler,
             std::unordered_map<std::string, std::string>& outParams) const;

private:
  struct Node {
    std::unordered_map<std::string, std::unique_ptr<Node>> staticChildren;
    std::unique_ptr<Node> paramChild;
    std::string paramName;
    std::unordered_map<HttpMethod, Handler> handlers;
  };

  std::unique_ptr<Node> root_;

  /** 按 '/' 分割路径，忽略空段（归一化斜杠） */
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
              std::unordered_map<std::string, std::string>& outParams) const;
};

} // namespace hps
