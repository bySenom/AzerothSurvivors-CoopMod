#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include "build_contract.hpp"
#include "extensions.hpp"
#include "crash_log.hpp"

namespace {
using ServerTick = void(__fastcall*)(void*, double);
ServerTick original_tick = nullptr;
std::atomic<std::uint64_t> ticks{0}, pending{0}, connections{0}, roster{0};
std::atomic<std::uintptr_t> server_address{0};
std::atomic<double> server_clock{0};
std::atomic<bool> layout_valid{false}, installed{false};
std::filesystem::path profile;
std::string failure;
HANDLE ready_event = nullptr;
std::array<std::atomic<azs::TickObserver>,8> observers{};

std::wstring environment(const wchar_t* name) {
    const auto size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    const auto written = GetEnvironmentVariableW(name, value.data(), size);
    value.resize(written);
    return value;
}

std::uint64_t read_u64(const void* source) {
    std::uint64_t value = 0;
    std::memcpy(&value, source, sizeof(value));
    return value;
}

bool readable_span(void* address, std::size_t bytes) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info)) return false;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return start <= end && bytes <= end - start;
}

void __fastcall on_server_tick(void* server, double clock) {
    original_tick(server, clock);
    server_address.store(reinterpret_cast<std::uintptr_t>(server), std::memory_order_relaxed);
    server_clock.store(clock, std::memory_order_relaxed);
    if (readable_span(server, azs::minimum_server_span)) {
        const auto bytes = static_cast<const unsigned char*>(server);
        const auto a = read_u64(bytes + azs::pending_connections_length);
        const auto b = read_u64(bytes + azs::auth_connections_length);
        const auto c = read_u64(bytes + azs::world_sessions_length);
        const bool valid = a < 4096 && b < 4096 && c < 65536;
        if (valid) {
            pending.store(a, std::memory_order_relaxed);
            connections.store(b, std::memory_order_relaxed);
            roster.store(c, std::memory_order_relaxed);
        }
        layout_valid.store(valid, std::memory_order_relaxed);
    } else {
        layout_valid.store(false, std::memory_order_relaxed);
    }
    ticks.fetch_add(1, std::memory_order_release);
    for(auto& slot:observers) if(const auto observer=slot.load(std::memory_order_acquire)) observer(server,clock);
}

std::string status() {
    std::ostringstream out;
    out << "{\"schema\":2,\"phase\":\"native-integration-probe\",\"pid\":" << GetCurrentProcessId()
        << ",\"hook_installed\":" << (installed.load() ? "true" : "false")
        << ",\"server_ticks\":" << ticks.load(std::memory_order_acquire)
        << ",\"server_clock\":" << server_clock.load()
        << ",\"server_address\":\"0x" << std::hex << server_address.load() << std::dec << "\""
        << ",\"layout_valid\":" << (layout_valid.load() ? "true" : "false")
        << ",\"pending_connections\":" << pending.load()
        << ",\"auth_connections\":" << connections.load()
        << ",\"world_sessions\":" << roster.load()
        << ",\"coop_implemented\":false,\"failure\":\"" << failure << "\"}\n";
    return out.str();
}

DWORD WINAPI serve_status(void*) {
    const auto name = L"\\\\.\\pipe\\AzerothCoop-" + std::to_wstring(GetCurrentProcessId());
    for (;;) {
        HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 16384, 128, 1000, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return GetLastError();
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            char request[64]{};
            DWORD read = 0;
            if (ReadFile(pipe, request, sizeof(request) - 1, &read, nullptr)) {
                auto message = status();
                if (std::string(request, read) != "status\n") {
                    message = "{\"error\":\"unsupported_request\"}\n";
                }
                DWORD written = 0;
                WriteFile(pipe, message.data(), static_cast<DWORD>(message.size()), &written, nullptr);
                FlushFileBuffers(pipe);
            }
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
    }
}

DWORD WINAPI initialize(void*) {
    const auto ready_name = L"Local\\AzerothCoopReady-" + std::to_wstring(GetCurrentProcessId());
    ready_event = CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str());
    profile = environment(L"AZS_COOP_PROFILE");
    if(!profile.empty()) azs::install_crash_log(profile);
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* target = reinterpret_cast<void*>(base + azs::server_tick_rva);
    if (profile.empty()) failure = "missing_isolated_profile";
    else if (!readable_span(target, azs::server_tick_prologue.size()) ||
             std::memcmp(target, azs::server_tick_prologue.data(), azs::server_tick_prologue.size()) != 0)
        failure = "unsupported_game_build_or_modified_hook_site";
    else {
        auto result = MH_Initialize();
        if (result == MH_OK) result = MH_CreateHook(target, &on_server_tick, reinterpret_cast<void**>(&original_tick));
        if (result == MH_OK) result = MH_EnableHook(target);
        if (result != MH_OK) failure = MH_StatusToString(result);
        else installed.store(true, std::memory_order_release);
    }
    if (!profile.empty()) {
        std::error_code error;
        std::filesystem::create_directories(profile, error);
        std::ofstream log(profile / "native-integration.jsonl", std::ios::app);
        log << status();
    }
    HANDLE thread = CreateThread(nullptr, 0, serve_status, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    if (ready_event) SetEvent(ready_event);
    return 0;
}
}

extern "C" __declspec(dllexport) bool __cdecl azs_register_tick_observer(azs::TickObserver observer) {
    if(!observer) return false;
    for(auto& slot:observers) {
        auto expected=static_cast<azs::TickObserver>(nullptr);
        if(slot.compare_exchange_strong(expected,observer,std::memory_order_acq_rel) || expected==observer) return true;
    }
    return false;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, initialize, nullptr, 0, nullptr);
        if (!thread) return FALSE;
        CloseHandle(thread);
    }
    return TRUE;
}
