// Generated from an inspected, SHA-256-verified game executable.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace azs {
inline constexpr char game_sha256[]="dc10453398ceb398076f6d3b6ac7859470de2ff7ccd68ac1d00a02f9f1e5209a";
inline constexpr std::uintptr_t server_tick_rva=0x58aee30;
inline constexpr std::array<unsigned char,32> server_tick_prologue={0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xec,0x28,0x06,0x00,0x00,0x48,0x8d,0xac,0x24,0x80,0x00,0x00,0x00,0x0f,0x29,0xbd,0x90,0x05};
inline constexpr std::size_t pending_connections_length=0xb00;
inline constexpr std::size_t auth_connections_length=0xb18;
inline constexpr std::size_t world_sessions_length=0xb30;
inline constexpr std::size_t minimum_server_span=0xb38;
}
