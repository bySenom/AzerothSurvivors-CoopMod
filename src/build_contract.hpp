#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace azs {
inline constexpr char game_sha256[] = "96e24dcd0e53fc13a6fdef5826aa46f77a29aa1aad4abd585adf04e94b2756d3";
inline constexpr std::uintptr_t server_tick_rva = 0x058798d0;
// Exact original instructions, including saves of nonvolatile Windows x64 registers.
inline constexpr std::array<unsigned char, 32> server_tick_prologue = {
    0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,
    0x48,0x81,0xec,0xe8,0x05,0x00,0x00,0x48,0x8d,0xac,0x24,0x80,
    0x00,0x00,0x00,0x0f,0x29,0xbd,0x50,0x05
};
// Observed native fields. The probe reads these on the server thread while its
// owning mutex is held. It does not mutate fields or assume a multiplayer ABI.
inline constexpr std::size_t pending_connections_length = 0x0a40;
inline constexpr std::size_t auth_connections_length = 0x0a58;
inline constexpr std::size_t world_sessions_length = 0x0a70;
inline constexpr std::size_t minimum_server_span = 0x0a78;
}
