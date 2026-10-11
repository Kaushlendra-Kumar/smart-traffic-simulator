#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace traffic::api {

/// SHA-1 digest (needed only for the RFC 6455 handshake; not used for security).
std::array<std::uint8_t, 20> sha1(std::string_view data);
std::string base64_encode(const std::uint8_t* data, std::size_t len);

}  // namespace traffic::api
