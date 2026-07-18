#include "http_request.h"

namespace hps {

std::string_view http_method_to_string(HttpMethod m) noexcept {
  switch (m) {
    case HttpMethod::GET:
      return "GET";
    case HttpMethod::HEAD:
      return "HEAD";
    case HttpMethod::POST:
      return "POST";
    case HttpMethod::PUT:
      return "PUT";
    case HttpMethod::DELETE:
      return "DELETE";
    case HttpMethod::CONNECT:
      return "CONNECT";
    case HttpMethod::OPTIONS:
      return "OPTIONS";
    case HttpMethod::TRACE:
      return "TRACE";
    case HttpMethod::PATCH:
      return "PATCH";
    default:
      return "UNKNOWN";
  }
}

HttpMethod string_to_http_method(std::string_view s) noexcept {
  if (s == "GET")
    return HttpMethod::GET;
  if (s == "HEAD")
    return HttpMethod::HEAD;
  if (s == "POST")
    return HttpMethod::POST;
  if (s == "PUT")
    return HttpMethod::PUT;
  if (s == "DELETE")
    return HttpMethod::DELETE;
  if (s == "CONNECT")
    return HttpMethod::CONNECT;
  if (s == "OPTIONS")
    return HttpMethod::OPTIONS;
  if (s == "TRACE")
    return HttpMethod::TRACE;
  if (s == "PATCH")
    return HttpMethod::PATCH;
  return HttpMethod::UNKNOWN;
}

void HttpRequest::clear() noexcept {
  method = HttpMethod::UNKNOWN;
  path.clear();
  query_string.clear();
  version = "HTTP/1.1";
  headers.clear();
  body.clear();
  path_params.clear();
  auth_user = AuthUser{};
}

} // namespace hps
