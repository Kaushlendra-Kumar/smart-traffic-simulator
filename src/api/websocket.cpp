#include "api/websocket.hpp"

#include "api/sha1.hpp"

namespace traffic::api::ws {

std::string accept_key(const std::string& client_key) {
  const auto digest = sha1(client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
  return base64_encode(digest.data(), digest.size());
}

std::string frame_header(std::uint8_t opcode, std::size_t len) {
  std::string h;
  h.push_back(static_cast<char>(0x80 | (opcode & 0x0F)));
  if (len < 126) {
    h.push_back(static_cast<char>(len));
  } else if (len <= 0xFFFF) {
    h.push_back(static_cast<char>(126));
    h.push_back(static_cast<char>((len >> 8) & 0xFF));
    h.push_back(static_cast<char>(len & 0xFF));
  } else {
    h.push_back(static_cast<char>(127));
    for (int i = 7; i >= 0; --i) h.push_back(static_cast<char>((static_cast<std::uint64_t>(len) >> (8 * i)) & 0xFF));
  }
  return h;
}

std::string client_frame(std::uint8_t opcode, const std::string& payload, std::uint32_t mask_key) {
  std::string f = frame_header(opcode, payload.size());
  f[1] = static_cast<char>(static_cast<unsigned char>(f[1]) | 0x80);
  const unsigned char mask[4] = {static_cast<unsigned char>(mask_key >> 24), static_cast<unsigned char>(mask_key >> 16),
                                 static_cast<unsigned char>(mask_key >> 8), static_cast<unsigned char>(mask_key)};
  for (unsigned char m : mask) f.push_back(static_cast<char>(m));
  for (std::size_t i = 0; i < payload.size(); ++i)
    f.push_back(static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i % 4]));
  return f;
}

FrameStatus parse_frame(std::string& buf, Frame& out, std::size_t max_payload, bool require_mask, std::string& error,
                        std::uint16_t& close_code) {
  if (buf.size() < 2) return FrameStatus::Incomplete;
  const auto b0 = static_cast<unsigned char>(buf[0]);
  const auto b1 = static_cast<unsigned char>(buf[1]);
  if (b0 & 0x70) {
    error = "reserved bits set (no extensions negotiated)";
    close_code = 1002;
    return FrameStatus::Error;
  }
  const bool masked = (b1 & 0x80) != 0;
  if (require_mask && !masked) {
    error = "client frames must be masked";
    close_code = 1002;
    return FrameStatus::Error;
  }
  std::uint64_t len = b1 & 0x7F;
  std::size_t pos = 2;
  if (len == 126) {
    if (buf.size() < 4) return FrameStatus::Incomplete;
    len = (static_cast<std::uint64_t>(static_cast<unsigned char>(buf[2])) << 8) | static_cast<unsigned char>(buf[3]);
    pos = 4;
  } else if (len == 127) {
    if (buf.size() < 10) return FrameStatus::Incomplete;
    len = 0;
    for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<unsigned char>(buf[2 + static_cast<std::size_t>(i)]);
    pos = 10;
  }
  const std::uint8_t opcode = b0 & 0x0F;
  if (opcode >= 0x8 && (len > 125 || !(b0 & 0x80))) {
    error = "control frames must be unfragmented and <= 125 bytes";
    close_code = 1002;
    return FrameStatus::Error;
  }
  if (len > max_payload) {
    error = "message too large";
    close_code = 1009;
    return FrameStatus::Error;
  }
  unsigned char mask[4] = {0, 0, 0, 0};
  if (masked) {
    if (buf.size() < pos + 4) return FrameStatus::Incomplete;
    for (std::size_t i = 0; i < 4; ++i) mask[i] = static_cast<unsigned char>(buf[pos + i]);
    pos += 4;
  }
  if (buf.size() < pos + len) return FrameStatus::Incomplete;
  out.fin = (b0 & 0x80) != 0;
  out.opcode = opcode;
  out.payload.assign(buf, pos, static_cast<std::size_t>(len));
  if (masked)
    for (std::size_t i = 0; i < out.payload.size(); ++i)
      out.payload[i] = static_cast<char>(static_cast<unsigned char>(out.payload[i]) ^ mask[i % 4]);
  buf.erase(0, pos + static_cast<std::size_t>(len));
  return FrameStatus::Ok;
}

}  // namespace traffic::api::ws
