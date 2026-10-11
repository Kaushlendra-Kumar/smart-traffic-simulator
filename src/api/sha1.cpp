#include "api/sha1.hpp"

#include <vector>

namespace traffic::api {

namespace {
std::uint32_t rol(std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
}  // namespace

std::array<std::uint8_t, 20> sha1(std::string_view data) {
  std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  std::vector<std::uint8_t> msg(data.begin(), data.end());
  const std::uint64_t bit_len = static_cast<std::uint64_t>(msg.size()) * 8;
  msg.push_back(0x80);
  while (msg.size() % 64 != 56) msg.push_back(0);
  for (int i = 7; i >= 0; --i) msg.push_back(static_cast<std::uint8_t>(bit_len >> (i * 8)));

  for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i)
      w[i] = (static_cast<std::uint32_t>(msg[chunk + 4 * i]) << 24) | (static_cast<std::uint32_t>(msg[chunk + 4 * i + 1]) << 16) |
             (static_cast<std::uint32_t>(msg[chunk + 4 * i + 2]) << 8) | msg[chunk + 4 * i + 3];
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      std::uint32_t f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
      const std::uint32_t temp = rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rol(b, 30);
      b = a;
      a = temp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  std::array<std::uint8_t, 20> out{};
  for (int i = 0; i < 5; ++i)
    for (int j = 0; j < 4; ++j) out[static_cast<std::size_t>(4 * i + j)] = static_cast<std::uint8_t>(h[i] >> (24 - 8 * j));
  return out;
}

std::string base64_encode(const std::uint8_t* data, std::size_t len) {
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((len + 2) / 3 * 4);
  for (std::size_t i = 0; i < len; i += 3) {
    std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
    if (i + 1 < len) n |= static_cast<std::uint32_t>(data[i + 1]) << 8;
    if (i + 2 < len) n |= data[i + 2];
    out.push_back(tbl[(n >> 18) & 63]);
    out.push_back(tbl[(n >> 12) & 63]);
    out.push_back(i + 1 < len ? tbl[(n >> 6) & 63] : '=');
    out.push_back(i + 2 < len ? tbl[n & 63] : '=');
  }
  return out;
}

}  // namespace traffic::api
