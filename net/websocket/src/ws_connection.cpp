#include "ws_connection.h"

#include "logger.h"

#include <poll.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hps {

WsConnection::WsConnection(std::shared_ptr<Connection> conn, MessageHandler on_message, CloseHandler on_close) :
    conn_(std::move(conn)), on_message_(std::move(on_message)), on_close_(std::move(on_close)) {}

void WsConnection::send(WsOpcode opcode, std::span<const char> payload, bool fin) {
  if (closed_) {
    return;
  }
  auto frame = ws_encode_frame(opcode, payload, fin);
  do_send(frame);
}

void WsConnection::close(uint16_t code, std::string_view reason) {
  if (closed_) {
    return;
  }
  closed_ = true;
  std::vector<char> close_payload(2 + reason.size());
  close_payload[0] = static_cast<char>((static_cast<uint32_t>(code) >> 8U) & 0xFFU);
  close_payload[1] = static_cast<char>(code & 0xFFU);
  if (!reason.empty()) {
    std::memcpy(close_payload.data() + 2, reason.data(), reason.size());
  }
  auto frame = ws_encode_frame(WsOpcode::CLOSE, close_payload);
  do_send(frame);
  conn_->close();
}

namespace {

std::size_t calc_frame_size(std::string_view data) {
  if (data.size() < 2) {
    return 0;
  }
  auto second = static_cast<uint8_t>(data[1]);
  std::size_t hdr_size = 2;
  auto payload_len = static_cast<uint64_t>(second & 0x7FU);
  if (payload_len == 126) {
    hdr_size = 4;
  } else if (payload_len == 127) {
    hdr_size = 10;
  }
  if ((second & 0x80U) != 0U) {
    hdr_size += 4;
  }
  return hdr_size + static_cast<std::size_t>(payload_len);
}

bool handle_frame(WsFrame& frame,
                  const std::function<void(WsFrame)>& on_message,
                  const std::function<void(uint16_t)>& on_close,
                  const std::function<void(const std::vector<char>&)>& do_send,
                  bool& closed) {
  if (frame.opcode == WsOpcode::CLOSE) {
    auto code = ws_close_code(frame);
    closed = true;
    if (on_close) {
      on_close(code.value_or(1005));
    }
    return false;
  }

  if (frame.opcode == WsOpcode::PING) {
    auto pong = ws_encode_frame(WsOpcode::PONG, frame.payload);
    do_send(pong);
    return true;
  }

  if (frame.opcode == WsOpcode::PONG) {
    return true;
  }

  if (on_message) {
    on_message(std::move(frame));
  }
  return true;
}

} // namespace

void WsConnection::start_event_loop() {
  std::string leftover;
  while (!closed_) {
    {
      struct pollfd pfd;
      pfd.fd = conn_->fd();
      pfd.events = POLLIN;
      int pret = poll(&pfd, 1, 100);
      if (pret <= 0) {
        break;
      }
    }

    auto n = conn_->read_from_fd();
    if (n < 0) {
      Logger::_error("WsConnection: read error");
      break;
    }
    if (n == 0) {
      Logger::_info("WsConnection: peer closed");
      break;
    }

    std::string local_buf;
    {
      std::lock_guard<std::mutex> rlock(conn_->read_mutex());
      local_buf = conn_->read_buffer();
      conn_->consume_read_buffer_locked(local_buf.size());
    }
    std::string feed;
    feed.swap(leftover);
    feed.append(local_buf.data(), local_buf.size());

    while (!feed.empty()) {
      auto frame_opt = ws_decode_frame(feed);
      if (!frame_opt.has_value()) {
        leftover = std::move(feed);
        break;
      }

      auto frame_size = calc_frame_size(feed);
      WsFrame frame = std::move(*frame_opt);

      if (feed.size() > frame_size) {
        feed = feed.substr(frame_size);
      } else {
        feed.clear();
      }

      bool keep_going =
        handle_frame(frame, on_message_, on_close_, [this](const std::vector<char>& f) { do_send(f); }, closed_);
      if (!keep_going) {
        conn_->close();
        return;
      }
    }
  }

  if (!closed_) {
    closed_ = true;
    if (on_close_) {
      on_close_(1006);
    }
    conn_->close();
  }
}

void WsConnection::do_send(const std::vector<char>& frame) {
  std::lock_guard<std::mutex> wlock(conn_->write_mutex());
  conn_->write_buffer().append(frame.data(), frame.size());
  conn_->write_to_fd_locked();
}

} // namespace hps
