#pragma once
// RFC 6455 framing helpers (pure functions, unit-testable without sockets).

#include <cstdint>
#include <string>

namespace traffic::api::ws {

enum Opcode : std::uint8_t { Continuation = 0x0, Text = 0x1, Binary = 0x2, Close = 0x8, Ping = 0x9, Pong = 0xA };

std::string accept_key(const std::string& client_key);
/// Header for an unmasked server->client frame with FIN set.
std::string frame_header(std::uint8_t opcode, std::size_t payload_len);
/// Full masked client->server frame (used by tests and tools).
std::string client_frame(std::uint8_t opcode, const std::string& payload, std::uint32_t mask_key = 0x12345678u);

struct Frame {
  bool fin = true;
  std::uint8_t opcode = Text;
  std::string payload;
};

enum class FrameStatus { Incomplete, Ok, Error };

/// Parses one frame from the front of `buffer` and consumes it on success.
/// `require_mask` is true for frames received by the server (clients must mask).
FrameStatus parse_frame(std::string& buffer, Frame& out, std::size_t max_payload, bool require_mask, std::string& error,
                        std::uint16_t& close_code);

}  // namespace traffic::api::ws
