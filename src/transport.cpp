#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <MinHook.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <deque>
#include <memory>
#include <unordered_map>
#include "build_contract.hpp"
#include "transport_contract.hpp"
#include "extensions.hpp"
#include "layout.hpp"

extern "C" { void* azs_new_pair_address = nullptr; }
namespace {
struct LocalStream { void* rx = nullptr; void* tx = nullptr; };
struct RustBytes { std::size_t capacity = 0; unsigned char* pointer = nullptr; std::size_t length = 0; };
extern "C" LocalStream* azs_create_pair(LocalStream* output);
using OpenLocal = LocalStream*(__fastcall*)(LocalStream*, void**, std::uint16_t);
using WriteLocal = void(__fastcall*)(void*, const void*, std::size_t);
using DrainLocal = RustBytes*(__fastcall*)(RustBytes*, void*);
using DropLocal = void(__fastcall*)(LocalStream*);
using Deallocate = void(__fastcall*)(void*, std::size_t, std::size_t);
OpenLocal open_original = nullptr;
WriteLocal write_original = nullptr;
DrainLocal drain_original = nullptr;
DropLocal drop_original = nullptr;
Deallocate free_original = nullptr;
std::uintptr_t image_base = 0;
std::atomic<std::uint64_t> sessions{0}, opened{0}, sent_bytes{0}, received_bytes{0};
std::atomic<unsigned> reserved_sessions{0};
std::atomic<std::uint64_t> bridge_errors{0};
std::atomic<bool> joining{false}, listening{false};
std::mutex configuration_mutex, error_mutex;
std::string remote_host, token;
std::uint16_t bridge_port = 0;
std::string failure;
std::string last_error;
struct UiPeer {
    std::string identity;
    std::mutex mutex;
    std::deque<std::vector<unsigned char>> outgoing;
    std::size_t bytes=0;
    std::atomic<bool> connected{true};
};
std::mutex ui_peers_mutex,ui_incoming_mutex;
std::vector<std::weak_ptr<UiPeer>> ui_peers;
struct UiEvent { std::uint64_t epoch; std::string text; };
struct UiWorld { std::uint64_t epoch=0; bool entered=false; };
std::unordered_map<std::string,UiWorld> ui_worlds;
std::deque<UiEvent> ui_incoming;
std::atomic<std::uint64_t> ui_epoch{0},ui_dropped{0};
std::array<unsigned char,16> client_identity{};
std::mutex remote_worlds_mutex;
std::unordered_map<std::uintptr_t,std::string> remote_worlds;
std::atomic<std::uint64_t> ui_sent{0},ui_received{0},ui_delivered{0};
std::atomic<unsigned> ui_connections{0};
std::vector<unsigned char> ui_frame(unsigned char kind,std::uint64_t epoch,const char* data=nullptr,std::size_t size=0) {
    std::vector<unsigned char> frame(13+size);
    const auto length=static_cast<std::uint32_t>(size+9);
    std::memcpy(frame.data(),&length,4); frame[4]=kind; std::memcpy(frame.data()+5,&epoch,8);
    if(size) std::memcpy(frame.data()+13,data,size);
    return frame;
}
bool enqueue_ui(const std::shared_ptr<UiPeer>& peer,const std::vector<unsigned char>& frame) {
    std::lock_guard lock(peer->mutex);
    if(peer->bytes+frame.size()>8*1024*1024 || peer->outgoing.size()>=128) { peer->connected.store(false); return false; }
    peer->outgoing.push_back(frame); peer->bytes+=frame.size(); return true;
}
void set_ui_world(const std::string& identity,std::uint64_t epoch,bool entered) {
    if(identity.empty()) return;
    std::lock_guard lock(ui_peers_mutex);
    const auto prior=ui_worlds[identity];
    if(prior.epoch==epoch && prior.entered==entered) return;
    ui_worlds[identity]={epoch,entered};
    const auto frame=ui_frame(entered?2:3,epoch);
    for(const auto& entry:ui_peers) if(const auto peer=entry.lock();peer && peer->identity==identity) enqueue_ui(peer,frame);
}
bool queue_ui(const char* identity,std::uint64_t epoch,const char* data,std::size_t size) {
    if(!listening.load() || size>2*1024*1024) return false;
    const auto frame=ui_frame(1,epoch,data,size);
    bool queued=false;
    std::lock_guard peers_lock(ui_peers_mutex);
    const auto world=ui_worlds.find(identity);
    if(world==ui_worlds.end() || !world->second.entered || world->second.epoch!=epoch) return false;
    for(auto i=ui_peers.begin();i!=ui_peers.end();) {
        const auto peer=i->lock();
        if(!peer || !peer->connected.load()) { i=ui_peers.erase(i); continue; }
        if(peer->identity==identity) queued=enqueue_ui(peer,frame) || queued;
        ++i;
    }
    return queued;
}
void __cdecl deliver_ui(void* server,double) {
    if(!joining.load()) return;
    std::deque<UiEvent> pending;
    { std::lock_guard lock(ui_incoming_mutex); pending.swap(ui_incoming); }
    auto* bytes=static_cast<unsigned char*>(server);
    void* object=nullptr; std::uintptr_t* vtable=nullptr;
    std::memcpy(&object,bytes+azs::layout::ui_object,8); std::memcpy(&vtable,bytes+azs::layout::ui_vtable,8);
    if(!object || !vtable) return;
    using Notify=void(__fastcall*)(void*,const char*,std::size_t);
    const auto callback=reinterpret_cast<Notify>(vtable[3]);
    const auto address=reinterpret_cast<std::uintptr_t>(callback);
    if(address<image_base || address>=image_base+azs::executable_image_size) return;
    // Original Server::tick calls this SAME borrowed UTF-8 event callback. Execute
    // it on the original server thread with its mutex held, never the socket thread.
    for(const auto& event:pending) {
        if(!event.epoch || event.epoch!=ui_epoch.load()) { ui_dropped.fetch_add(1); continue; }
        callback(object,event.text.data(),event.text.size()); ui_delivered.fetch_add(1);
    }
}
void record_error(const std::exception& error) {
    bridge_errors.fetch_add(1);
    std::lock_guard lock(error_mutex); last_error=error.what();
}

template<class T> T native(std::uintptr_t rva) { return reinterpret_cast<T>(image_base + rva); }
bool service_allowed(std::uint16_t port) { return port == 3724 || (port >= 8085 && port <= 8096); }
void check(bool condition, const char* what) { if (!condition) throw std::runtime_error(what); }
std::string rpc_base() {
    const auto name = L"\\\\.\\pipe\\AzerothCoop-" + std::to_wstring(GetCurrentProcessId());
    HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    check(pipe != INVALID_HANDLE_VALUE, "base_module_status_unavailable");
    DWORD size = 0; char data[16384]{};
    bool ok = WriteFile(pipe, "status\n", 7, &size, nullptr) && ReadFile(pipe, data, sizeof(data), &size, nullptr);
    CloseHandle(pipe); check(ok, "base_module_status_failed");
    return std::string(data, size);
}
void* server_arc() {
    auto data = rpc_base();
    const std::string key = "\"server_address\":\"0x";
    const auto position = data.find(key);
    check(position != std::string::npos, "server_address_missing");
    auto address = std::stoull(data.substr(position + key.size()), nullptr, 16);
    check(address >= 0x10000, "server_not_ready");
    return reinterpret_cast<void*>(address - 0x18);
}
std::vector<unsigned char> drain(void* arc) {
    RustBytes value;
    drain_original(&value, static_cast<unsigned char*>(arc) + 0x10);
    check(value.length <= value.capacity && value.length <= 32 * 1024 * 1024, "invalid_native_byte_vector");
    std::vector<unsigned char> bytes;
    if (value.length) bytes.assign(value.pointer, value.pointer + value.length);
    if (value.capacity) free_original(value.pointer, value.capacity, 1);
    return bytes;
}
void put(void* arc, const void* data, std::size_t size) {
    if (size) write_original(static_cast<unsigned char*>(arc) + 0x10, data, size);
}
bool closed(void* arc) {
    unsigned char value = 0;
    SIZE_T read = 0;
    return !ReadProcessMemory(GetCurrentProcess(), static_cast<unsigned char*>(arc) + 0x48,
            &value, sizeof(value), &read) || value != 0;
}
bool transfer(SOCKET socket, const void* data, std::size_t length, bool receiving) {
    auto* cursor = static_cast<char*>(const_cast<void*>(data));
    while (length) {
        const auto amount = static_cast<int>((std::min)(length, static_cast<std::size_t>(1 << 20)));
        const auto n = receiving ? recv(socket, cursor, amount, 0) : send(socket, cursor, amount, 0);
        if (n <= 0) return false;
        cursor += n; length -= n;
    }
    return true;
}
void configure_socket(SOCKET socket) {
    int one = 1; DWORD timeout = 5000;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&one), sizeof(one));
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
}
std::array<unsigned char,120> hello(std::uint16_t service, const std::string& room_token,const std::array<unsigned char,16>& identity) {
    std::array<unsigned char,120> bytes{};
    std::memcpy(bytes.data(), "AZSC", 4); bytes[4] = 2;
    bytes[6] = static_cast<unsigned char>(service); bytes[7] = static_cast<unsigned char>(service >> 8);
    std::memcpy(bytes.data() + 8, azs::game_sha256, 64);
    std::memcpy(bytes.data() + 72, room_token.data(), (std::min)(room_token.size(), std::size_t(32)));
    std::memcpy(bytes.data()+104,identity.data(),identity.size());
    return bytes;
}
SOCKET connect_bridge(std::uint16_t service) {
    std::string host, room_token; std::uint16_t port;
    { std::lock_guard lock(configuration_mutex); host = remote_host; room_token = token; port = bridge_port; }
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    check(getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &addresses) == 0, "host_lookup_failed");
    SOCKET connected = INVALID_SOCKET;
    for (auto* address = addresses; address; address = address->ai_next) {
        SOCKET candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate == INVALID_SOCKET) continue;
        configure_socket(candidate);
        if (connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) { connected = candidate; break; }
        closesocket(candidate);
    }
    freeaddrinfo(addresses);
    check(connected != INVALID_SOCKET, "host_connection_failed");
    auto request = hello(service, room_token,client_identity);
    std::array<unsigned char,8> response{};
    if (!transfer(connected, request.data(), request.size(), false) || !transfer(connected, response.data(), response.size(), true)
        || std::memcmp(response.data(), "AZSO", 4) || response[4] != 0) {
        closesocket(connected); throw std::runtime_error("bridge_handshake_rejected");
    }
    return connected;
}
void host_ui(SOCKET socket,const std::string& identity) {
    auto peer=std::make_shared<UiPeer>();
    peer->identity=identity;
    { std::lock_guard lock(ui_peers_mutex); ui_peers.push_back(peer);
      const auto world=ui_worlds.find(identity);
      if(world!=ui_worlds.end() && world->second.entered) enqueue_ui(peer,ui_frame(2,world->second.epoch)); }
    ui_connections.fetch_add(1);
    while(peer->connected.load()) {
        std::deque<std::vector<unsigned char>> pending;
        { std::lock_guard lock(peer->mutex); pending.swap(peer->outgoing); peer->bytes=0; }
        for(const auto& frame:pending) {
            if(!transfer(socket,frame.data(),frame.size(),false)) { peer->connected.store(false); break; }
            ui_sent.fetch_add(1);
        }
        fd_set set; FD_ZERO(&set); FD_SET(socket,&set); timeval timeout{0,2000};
        const auto ready=select(0,&set,nullptr,nullptr,&timeout);
        if(ready==SOCKET_ERROR || ready>0) break; // UI event channel is host -> guest only.
    }
    peer->connected.store(false); shutdown(socket,SD_BOTH); closesocket(socket); ui_connections.fetch_sub(1);
}
void join_ui() {
    SOCKET socket=INVALID_SOCKET;
    try {
        socket=connect_bridge(azs::ui_service); ui_connections.fetch_add(1);
        // A connected UI channel can legitimately be quiet for a whole run.
        DWORD no_timeout=0; setsockopt(socket,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&no_timeout),sizeof(no_timeout));
        for(;;) {
            std::uint32_t length=0;
            if(!transfer(socket,&length,4,true)) break;
            check(length>=9 && length<=2*1024*1024+9,"invalid_ui_frame_length");
            std::vector<unsigned char> frame(length);
            if(!transfer(socket,frame.data(),frame.size(),true)) break;
            std::uint64_t epoch=0; std::memcpy(&epoch,frame.data()+1,8);
            if(frame[0]==2) { ui_epoch.store(epoch); continue; }
            if(frame[0]==3) { ui_epoch.store(0); std::lock_guard lock(ui_incoming_mutex); ui_incoming.clear(); continue; }
            check(frame[0]==1,"unsupported_ui_event_version");
            if(!epoch || epoch!=ui_epoch.load()) { ui_dropped.fetch_add(1); continue; }
            { std::lock_guard lock(ui_incoming_mutex);
              check(ui_incoming.size()<128,"ui_event_queue_overflow");
              ui_incoming.push_back({epoch,std::string(reinterpret_cast<const char*>(frame.data()+9),frame.size()-9)}); }
            ui_received.fetch_add(1);
        }
    } catch(const std::exception& error) { record_error(error); }
    ui_epoch.store(0);
    { std::lock_guard lock(ui_incoming_mutex); ui_incoming.clear(); }
    if(socket!=INVALID_SOCKET) { shutdown(socket,SD_BOTH); closesocket(socket); ui_connections.fetch_sub(1); }
}
// Host endpoints: socket -> original stream.tx, original stream.rx -> socket.
// Join endpoints: socket -> original stream.rx, original stream.tx -> socket.
void pump(SOCKET socket, LocalStream stream, bool client_side) {
    sessions.fetch_add(1); opened.fetch_add(1);
    void* inbound = client_side ? stream.rx : stream.tx;
    void* outbound = client_side ? stream.tx : stream.rx;
    std::array<unsigned char,65536> buffer;
    try {
        while (!closed(inbound) && !closed(outbound)) {
            auto output = drain(outbound);
            if (!output.empty()) {
                if (!transfer(socket, output.data(), output.size(), false)) break;
                sent_bytes.fetch_add(output.size());
            }
            fd_set readable; FD_ZERO(&readable); FD_SET(socket, &readable);
            timeval timeout{0,2000};
            const auto ready = select(0, &readable, nullptr, nullptr, &timeout);
            if (ready == SOCKET_ERROR) break;
            if (ready > 0) {
                const auto n = recv(socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
                if (n <= 0) break;
                put(inbound, buffer.data(), static_cast<std::size_t>(n));
                received_bytes.fetch_add(n);
            }
        }
    } catch (const std::exception& error) { record_error(error); }
    shutdown(socket, SD_BOTH); closesocket(socket);
    drop_original(&stream);
    sessions.fetch_sub(1);
}
void accept_session(SOCKET socket) {
    struct Reservation { ~Reservation() { reserved_sessions.fetch_sub(1); } } reservation;
    configure_socket(socket);
    std::array<unsigned char,120> request{};
    if (!transfer(socket, request.data(), request.size(), true)) { closesocket(socket); return; }
    const auto service = static_cast<std::uint16_t>(request[6] | (request[7] << 8));
    std::array<unsigned char,16> identity_bytes{};
    std::memcpy(identity_bytes.data(),request.data()+104,16);
    auto expected = hello(service, token,identity_bytes);
    std::ostringstream identity_stream;
    for(const auto value:identity_bytes) identity_stream<<std::hex<<std::setfill('0')<<std::setw(2)<<static_cast<unsigned>(value);
    const auto identity=identity_stream.str();
    unsigned char mismatch = 0;
    for (std::size_t i = 0; i < request.size(); ++i) mismatch |= request[i] ^ expected[i];
    std::array<unsigned char,8> response{'A','Z','S','O',0,0,0,0};
    if (mismatch || (!service_allowed(service) && service!=azs::ui_service)) {
        response[4] = 1; transfer(socket, response.data(), response.size(), false); closesocket(socket); return;
    }
    LocalStream stream;
    try {
        if(service==azs::ui_service) {
            if(!transfer(socket,response.data(),response.size(),false)) { closesocket(socket); return; }
            host_ui(socket,identity); return;
        }
        void* arc = server_arc(); open_original(&stream, &arc, service);
        const auto key=reinterpret_cast<std::uintptr_t>(stream.tx);
        if(service>=8085 && service<=8096) { std::lock_guard lock(remote_worlds_mutex); remote_worlds[key]=identity; }
        if (!transfer(socket, response.data(), response.size(), false)) { drop_original(&stream); closesocket(socket); return; }
        pump(socket, stream, false);
        { std::lock_guard lock(remote_worlds_mutex); remote_worlds.erase(key); }
    } catch (const std::exception& error) { record_error(error); closesocket(socket); }
}
void host_loop(SOCKET listener) {
    for (;;) {
        SOCKET connection = accept(listener, nullptr, nullptr);
        if (connection == INVALID_SOCKET) break;
        if (reserved_sessions.fetch_add(1) >= 16) { reserved_sessions.fetch_sub(1); closesocket(connection); continue; }
        try { std::thread(accept_session, connection).detach(); }
        catch (...) { reserved_sessions.fetch_sub(1); closesocket(connection); }
    }
    closesocket(listener); listening.store(false);
}
LocalStream* __fastcall open_redirected(LocalStream* result, void** context, std::uint16_t service) {
    if (!joining.load()) return open_original(result, context, service);
    azs_create_pair(result);
    // The original client owns the returned references. The bridge owns a clone
    // until its socket closes, using the native drop function for both endpoints.
    InterlockedIncrement64(static_cast<volatile LONG64*>(result->rx));
    InterlockedIncrement64(static_cast<volatile LONG64*>(result->tx));
    const auto stream = *result;
    try {
        std::thread([stream,service] {
            try { pump(connect_bridge(service), stream, true); }
            catch (const std::exception& error) { record_error(error); LocalStream copy = stream; drop_original(&copy); }
        }).detach();
    } catch (const std::exception& error) {
        record_error(error); LocalStream copy=stream; drop_original(&copy);
    }
    return result;
}
std::string state() {
    std::ostringstream out;
    out << "{\"schema\":1,\"phase\":\"native-byte-stream-bridge\",\"pid\":" << GetCurrentProcessId()
        << ",\"listening\":" << (listening.load() ? "true" : "false")
        << ",\"joining\":" << (joining.load() ? "true" : "false")
        << ",\"active_streams\":" << sessions.load() << ",\"opened_streams\":" << opened.load()
        << ",\"sent_bytes\":" << sent_bytes.load() << ",\"received_bytes\":" << received_bytes.load()
        << ",\"bridge_errors\":" << bridge_errors.load()
        << ",\"ui_connections\":"<<ui_connections.load()<<",\"ui_sent\":"<<ui_sent.load()
        << ",\"ui_received\":"<<ui_received.load()<<",\"ui_delivered\":"<<ui_delivered.load()
        << ",\"ui_world_entered\":"<<(ui_epoch.load()?"true":"false")<<",\"ui_dropped\":"<<ui_dropped.load()
        << ",\"shared_simulation_verified\":false,\"failure\":\"" << failure << "\"}\n";
    return out.str();
}
std::string command(const std::string& text) {
    std::istringstream input(text); std::string action; input >> action;
    if (action == "status") return state();
    check(failure.empty(), "native_transport_contract_failed");
    if (action == "probe" || action == "probe-redirected") {
        unsigned port; input >> port;
        check(input && port <= 65535 && service_allowed(static_cast<std::uint16_t>(port)), "invalid_service");
        std::string payload; input >> payload;
        check(payload.size()%2 == 0 && payload.size() <= 65536, "invalid_probe_payload");
        std::vector<unsigned char> request;
        for(std::size_t i=0;i<payload.size();i+=2) {
            std::size_t used=0; const auto value=std::stoul(payload.substr(i,2),&used,16);
            check(used==2,"invalid_probe_hex"); request.push_back(static_cast<unsigned char>(value));
        }
        LocalStream stream; void* arc = server_arc();
        const auto callback=action=="probe-redirected" ? native<OpenLocal>(0x05878ce0) : open_original;
        callback(&stream, &arc, static_cast<std::uint16_t>(port));
        if(!request.empty()) put(stream.tx,request.data(),request.size());
        std::vector<unsigned char> bytes;
        for (int i = 0; i < 200 && bytes.empty(); ++i) { bytes = drain(stream.rx); if (bytes.empty()) Sleep(5); }
        drop_original(&stream);
        std::ostringstream out; out << "{\"native_service\":" << port << ",\"bytes\":" << bytes.size() << ",\"hex\":\"";
        for (auto value : bytes) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(value);
        out << "\"}\n"; return out.str();
    }
    std::string address, room_token; unsigned port;
    input >> address >> port >> room_token;
    check(input && port > 0 && port <= 65535 && room_token.size() == 32, "invalid_bridge_configuration");
    if (action == "host") {
        check(!listening.load() && !joining.load(), "bridge_already_configured");
        sockaddr_in local{}; local.sin_family = AF_INET; local.sin_port = htons(static_cast<u_short>(port));
        check(inet_pton(AF_INET, address.c_str(), &local.sin_addr) == 1, "bind_address_must_be_ipv4");
        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        check(listener != INVALID_SOCKET, "creating_listener_failed");
        if (bind(listener, reinterpret_cast<sockaddr*>(&local), sizeof(local)) || listen(listener, 8)) {
            closesocket(listener); throw std::runtime_error("binding_listener_failed");
        }
        { std::lock_guard lock(configuration_mutex); remote_host = address; bridge_port = static_cast<std::uint16_t>(port); token = room_token; }
        listening.store(true); std::thread(host_loop, listener).detach(); return state();
    }
    if (action == "join") {
        check(!listening.load() && !joining.load(), "bridge_already_configured");
        { std::lock_guard lock(configuration_mutex); remote_host = address; bridge_port = static_cast<std::uint16_t>(port); token = room_token; }
        auto result = MH_Initialize();
        check(result == MH_OK || result == MH_ERROR_ALREADY_INITIALIZED, "hook_library_failed");
        result = MH_CreateHook(native<void*>(0x05878ce0), &open_redirected, reinterpret_cast<void**>(&open_original));
        check(result == MH_OK, "factory_hook_failed");
        check(MH_EnableHook(native<void*>(0x05878ce0)) == MH_OK, "factory_hook_enable_failed");
        const auto observe=azs::extension<azs::RegisterTickObserver>("azs_register_tick_observer");
        check(observe && observe(&deliver_ui),"base_tick_observer_unavailable");
        joining.store(true); std::thread(join_ui).detach(); return state();
    }
    throw std::runtime_error("unsupported_transport_command");
}
DWORD WINAPI control(void*) {
    const auto name = L"\\\\.\\pipe\\AzerothCoopTransport-" + std::to_wstring(GetCurrentProcessId());
    for (;;) {
        HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,1,16384,512,1000,nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return GetLastError();
        if (ConnectNamedPipe(pipe,nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            char request[512]{}; DWORD count = 0;
            if (ReadFile(pipe,request,sizeof(request)-1,&count,nullptr)) {
                std::string response;
                try { response = command(std::string(request,count)); }
                catch (const std::exception& error) { response = "{\"error\":\"" + std::string(error.what()) + "\"}\n"; }
                WriteFile(pipe,response.data(),static_cast<DWORD>(response.size()),&count,nullptr); FlushFileBuffers(pipe);
            }
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
    }
}
DWORD WINAPI initialize(void*) {
    image_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image_base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image_base + dos->e_lfanew);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt->FileHeader.TimeDateStamp != azs::executable_timestamp ||
        nt->OptionalHeader.SizeOfImage != azs::executable_image_size) failure = "unsupported_native_image";
    if(failure.empty()) for(const auto& entry:azs::transport_entries) {
        if(entry.rva + entry.bytes.size() > nt->OptionalHeader.SizeOfImage ||
           std::memcmp(native<void*>(entry.rva),entry.bytes.data(),entry.bytes.size())) {
            failure = "native_transport_signature_mismatch"; break;
        }
    }
    open_original = native<OpenLocal>(azs::open_rva);
    write_original = native<WriteLocal>(azs::write_rva); drain_original = native<DrainLocal>(azs::drain_rva);
    drop_original = native<DropLocal>(azs::drop_rva); free_original = native<Deallocate>(azs::free_rva);
    azs_new_pair_address = native<void*>(azs::pair_rva);
    WSADATA data{}; if (WSAStartup(MAKEWORD(2,2),&data)) failure = "winsock_initialization_failed";
    if(BCryptGenRandom(nullptr,client_identity.data(),static_cast<ULONG>(client_identity.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)!=0) failure="client_identity_generation_failed";
    HANDLE thread = CreateThread(nullptr,0,control,nullptr,0,nullptr); if(thread) CloseHandle(thread);
    return 0;
}
}
extern "C" __declspec(dllexport) bool __cdecl azs_configure_transport(const char* request,char* error,std::size_t capacity) {
    try {
        if(!request || std::strlen(request)>500) throw std::runtime_error("invalid_transport_request");
        command(request);
        if(error && capacity) error[0]=0;
        return true;
    } catch(const std::exception& e) {
        if(error && capacity) {
            const auto count=(std::min)(capacity-1,std::strlen(e.what()));
            std::memcpy(error,e.what(),count); error[count]=0;
        }
        return false;
    }
}
extern "C" __declspec(dllexport) bool __cdecl azs_send_native_ui_to(const char* identity,std::uint64_t epoch,const char* text,std::size_t size) {
    return identity && text && queue_ui(identity,epoch,text,size);
}
extern "C" __declspec(dllexport) void __cdecl azs_set_ui_world(const char* identity,std::uint64_t epoch,bool entered) {
    if(identity) set_ui_world(identity,epoch,entered);
}
extern "C" __declspec(dllexport) bool __cdecl azs_remote_identity(std::uintptr_t queue,char* output) {
    std::lock_guard lock(remote_worlds_mutex);
    const auto found=remote_worlds.find(queue);
    if(found==remote_worlds.end() || !output) return false;
    std::memcpy(output,found->second.c_str(),33); return true;
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) {
    if(reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(module); HANDLE thread=CreateThread(nullptr,0,initialize,nullptr,0,nullptr); if(!thread) return FALSE; CloseHandle(thread); }
    return TRUE;
}
