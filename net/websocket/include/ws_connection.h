#pragma once

#include "connection.h"
#include "websocket.h"

#include <functional>
#include <memory>

namespace hps {

class WsConnection : public std::enable_shared_from_this<WsConnection> {
public:
  using MessageHandler = std::function<void(WsFrame frame)>;
  using CloseHandler = std::function<void(uint16_t code)>;

  WsConnection(std::shared_ptr<Connection> conn, MessageHandler on_message, CloseHandler on_close);

  void send(WsOpcode opcode, std::span<const char> payload, bool fin = true);

  void close(uint16_t code = 1000, std::string_view reason = {});

  void set_message_handler(MessageHandler handler) { on_message_ = std::move(handler); }

  void set_close_handler(CloseHandler handler) { on_close_ = std::move(handler); }

  void start_event_loop();

private:
  void do_send(const std::vector<char>& frame);

  std::shared_ptr<Connection> conn_;
  MessageHandler on_message_;
  CloseHandler on_close_;
  bool closed_{false};
};

} // namespace hps
