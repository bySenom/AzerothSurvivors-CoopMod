#include <windows.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "build_contract.hpp"

namespace fs = std::filesystem;
namespace {
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE v = nullptr) : value(v) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string(message) + " (Windows error " + std::to_string(GetLastError()) + ")");
}
std::string sha256(const fs::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0, "SHA256 provider");
    DWORD size = 0, written = 0;
    const auto property = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &written, 0);
    if (property != 0) { BCryptCloseAlgorithmProvider(algorithm, 0); throw std::runtime_error("SHA256 object size"); }
    std::vector<unsigned char> object(size), buffer(1024 * 1024), digest(32);
    if (BCryptCreateHash(algorithm, &hash, object.data(), size, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0); throw std::runtime_error("SHA256 initialization");
    }
    std::ifstream input(path, std::ios::binary);
    bool ok = static_cast<bool>(input);
    while (input && ok) {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        const auto length = input.gcount();
        if (length) ok = BCryptHashData(hash, buffer.data(), static_cast<ULONG>(length), 0) == 0;
    }
    ok = ok && input.eof() && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
    require(ok, "Hashing game executable");
    std::ostringstream out;
    for (auto byte : digest) out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    return out.str();
}
struct CaseInsensitive {
    bool operator()(const std::wstring& a, const std::wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; }
};
std::vector<wchar_t> environment_block(const fs::path& profile, const fs::path& game,bool ingame_lobby=false) {
    std::map<std::wstring, std::wstring, CaseInsensitive> vars;
    auto* source = GetEnvironmentStringsW();
    require(source != nullptr, "Reading environment");
    for (auto* p = source; *p; p += std::wcslen(p) + 1) {
        std::wstring entry(p);
        const auto split = entry.find(L'=', entry[0] == L'=' ? 1 : 0);
        if (split != std::wstring::npos) vars[entry.substr(0, split)] = entry.substr(split + 1);
    }
    FreeEnvironmentStringsW(source);
    vars[L"AZS_COOP_PROFILE"] = profile.wstring();
    vars[L"AZS_COOP_ROLE"] = L"probe";
    if(ingame_lobby) {
        vars[L"AZS_COOP_ROLE"]=L"play";
        vars[L"WOW_ROGUE_GLUE"]=L"0"; // native hero/zone picker, before its picked confirmation
    }
    vars[L"WOW_ROGUE_SAVE"] = (profile / "characters.txt").wstring();
    vars[L"APPDATA"] = (profile / "AppData/Roaming").wstring();
    vars[L"LOCALAPPDATA"] = (profile / "AppData/Local").wstring();
    vars[L"BENILLA_HOME"] = (profile / "benilla").wstring();
    vars[L"WOW_DATA"] = (game.parent_path() / "Data").wstring();
    vars[L"WOW_WIN"] = L"1280x720";
    vars[L"WOW_ROGUE_UPDATE"] = L"0";
    vars[L"WOW_ROGUE_UPDATE_AUTO"] = L"0";
    vars[L"WOW_ROGUE_UPDATE_URL"] = L"http://127.0.0.1:9/disabled-update.json";
    vars[L"WOW_ROGUE_DATA_URL"] = L"http://127.0.0.1:9/disabled-data.json";
    std::vector<wchar_t> output;
    for (const auto& [key, value] : vars) {
        const auto line = key + L"=" + value;
        output.insert(output.end(), line.begin(), line.end()); output.push_back(L'\0');
    }
    output.push_back(L'\0');
    return output;
}
std::uintptr_t remote_module(DWORD pid, const std::wstring& filename) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
        if (snapshot.value != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
            if (Module32FirstW(snapshot.value, &entry)) do {
                if (_wcsicmp(entry.szModule, filename.c_str()) == 0) return reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
            } while (Module32NextW(snapshot.value, &entry));
        }
        Sleep(10);
    }
    DWORD code=STILL_ACTIVE;
    Handle child(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));
    if(child.value) GetExitCodeProcess(child.value,&code);
    throw std::runtime_error("Target Windows loader module was not loaded (process " + std::to_string(pid) +
        ", exit " + std::to_string(code) + ")");
}
void inject(HANDLE process, DWORD pid, const fs::path& dll) {
    auto* local_load = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    require(local_load != nullptr, "Resolving LoadLibraryW");
    HMODULE actual_module = nullptr;
    require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(local_load), &actual_module), "Resolving forwarded loader module");
    wchar_t module_path[32768]{};
    require(GetModuleFileNameW(actual_module, module_path, 32768) != 0, "Loader module path");
    const auto remote_base = remote_module(pid, fs::path(module_path).filename().wstring());
    const auto offset = reinterpret_cast<std::uintptr_t>(local_load) - reinterpret_cast<std::uintptr_t>(actual_module);
    fs::path module_directory = dll.parent_path() / "loaded";
    const auto cache_size=GetEnvironmentVariableW(L"AZS_COOP_MODULE_CACHE",nullptr,0);
    if(cache_size) {
        std::wstring cache(cache_size,L'\0');
        cache.resize(GetEnvironmentVariableW(L"AZS_COOP_MODULE_CACHE",cache.data(),cache_size));
        module_directory=fs::absolute(cache);
    }
    fs::create_directories(module_directory);
    const auto digest=sha256(dll);
    const auto loaded_copy = module_directory / (fs::path(digest).wstring() + L".dll");
    if (!fs::exists(loaded_copy)) fs::copy_file(dll, loaded_copy);
    if(sha256(loaded_copy)!=digest) throw std::runtime_error("Loaded mod cache differs from the verified package");
    const auto canonical_name = fs::canonical(loaded_copy).wstring();
    // Installed version folders plus immutable DLL names can exceed MAX_PATH.
    // The target game's own manifest is outside our control; give its loader
    // an explicit extended-length path instead of relying on that manifest.
    const auto name = canonical_name.starts_with(L"\\\\?\\") ? canonical_name :
        canonical_name.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + canonical_name.substr(2) : L"\\\\?\\" + canonical_name;
    const auto bytes = (name.size() + 1) * sizeof(wchar_t);
    auto* remote_name = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    require(remote_name != nullptr, "Allocating module path");
    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remote_name, name.c_str(), bytes, &written) || written != bytes) {
        VirtualFreeEx(process, remote_name, 0, MEM_RELEASE); throw std::runtime_error("Writing module path");
    }
    Handle thread(CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_base + offset), remote_name, 0, nullptr));
    if (!thread.value) { VirtualFreeEx(process, remote_name, 0, MEM_RELEASE); throw std::runtime_error("Loading co-op module"); }
    const auto result = WaitForSingleObject(thread.value, 15000);
    // The path must stay allocated if the loader is still using it.
    if (result == WAIT_OBJECT_0) VirtualFreeEx(process, remote_name, 0, MEM_RELEASE);
    require(result == WAIT_OBJECT_0, "Waiting for module load");
    DWORD exit = 0;
    require(GetExitCodeThread(thread.value, &exit) && exit != 0, "Game rejected co-op module");
}
std::string query(DWORD pid, const std::wstring& prefix = L"AzerothCoop-", const std::string& request = "status\n") {
    const auto name = L"\\\\.\\pipe\\" + prefix + std::to_wstring(pid);
    Handle pipe;
    for (int i = 0; i < 100; ++i) {
        pipe.value = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe.value != INVALID_HANDLE_VALUE) break;
        WaitNamedPipeW(name.c_str(), 100); Sleep(10);
    }
    require(pipe.value != INVALID_HANDLE_VALUE, "Opening game module status pipe");
    DWORD written = 0;
    require(WriteFile(pipe.value, request.data(), static_cast<DWORD>(request.size()), &written, nullptr) && written == request.size(), "Requesting module status");
    char buffer[16384]{}; DWORD read = 0;
    require(ReadFile(pipe.value, buffer, sizeof(buffer), &read, nullptr), "Reading module status");
    return std::string(buffer, read);
}
std::wstring environment_value(const wchar_t* key) {
    const auto size=GetEnvironmentVariableW(key,nullptr,0);
    if(!size) return {};
    std::wstring value(size,L'\0');
    value.resize(GetEnvironmentVariableW(key,value.data(),size)); return value;
}
void start_crash_watcher(DWORD pid) {
    const auto watcher=environment_value(L"AZS_COOP_WATCHER");
    const auto session=environment_value(L"AZS_COOP_CRASH_DIR");
    if(watcher.empty() || session.empty()) return; // Development CLI without desktop package.
    const auto ready_name=L"Local\\AzerothCoopWatchReady-"+std::to_wstring(pid);
    Handle ready(CreateEventW(nullptr,TRUE,FALSE,ready_name.c_str()));
    require(ready.value!=nullptr,"Creating crash watcher readiness event");
    auto command=L"\""+watcher+L"\" /watch-game "+std::to_wstring(pid)+L" /session \""+session+L"\" /quiet";
    STARTUPINFOW startup{}; startup.cb=sizeof(startup);
    PROCESS_INFORMATION helper{};
    // No inherited stdout/stderr handles: the launcher must be able to exit
    // independently of this long-lived observer.
    require(CreateProcessW(watcher.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&helper),"Starting crash watcher");
    Handle helper_process(helper.hProcess),helper_thread(helper.hThread);
    HANDLE waits[]{ready.value,helper_process.value};
    require(WaitForMultipleObjects(2,waits,FALSE,10000)==WAIT_OBJECT_0,"Crash watcher readiness");
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 4 && (std::wstring(argv[1]) == L"attach" || std::wstring(argv[1]) == L"attach-gameplay")) {
            const auto pid = static_cast<DWORD>(std::stoul(argv[2]));
            const auto current = query(pid);
            if (current.find("\"hook_installed\":true") == std::string::npos) throw std::runtime_error("The target is not an initialized isolated native probe");
            const auto dll = fs::canonical(argv[3]);
            Handle process(OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid));
            require(process.value != nullptr, "Opening isolated game");
            wchar_t image[32768]{}; DWORD size = 32768;
            require(QueryFullProcessImageNameW(process.value, 0, image, &size), "Verifying target game path");
            if (sha256(fs::path(image)) != azs::game_sha256) throw std::runtime_error("Target build SHA256 differs");
            inject(process.value, pid, dll);
            std::cout << query(pid, std::wstring(argv[1]) == L"attach-gameplay" ? L"AzerothCoopGameplay-" : L"AzerothCoopTransport-"); return 0;
        }
        if (argc >= 4 && (std::wstring(argv[1]) == L"transport" || std::wstring(argv[1]) == L"gameplay" || std::wstring(argv[1]) == L"lobby")) {
            std::string request;
            for(int i=3;i<argc;++i) {
                if(i>3) request+=' ';
                const auto chars = std::wcslen(argv[i]);
                const auto count = WideCharToMultiByte(CP_UTF8,0,argv[i],static_cast<int>(chars),nullptr,0,nullptr,nullptr);
                const auto offset=request.size(); request.resize(offset+count);
                WideCharToMultiByte(CP_UTF8,0,argv[i],static_cast<int>(chars),request.data()+offset,count,nullptr,nullptr);
            }
            request+='\n';
            const auto prefix=std::wstring(argv[1])==L"lobby"?L"AzerothCoopLobby-":
                std::wstring(argv[1])==L"gameplay"?L"AzerothCoopGameplay-":L"AzerothCoopTransport-";
            std::cout << query(static_cast<DWORD>(std::stoul(argv[2])),prefix,request); return 0;
        }
        if (argc == 3 && std::wstring(argv[1]) == L"status") {
            std::cout << query(static_cast<DWORD>(std::stoul(argv[2]))); return 0;
        }
        const bool test_client=argc==9 && std::wstring(argv[1])==L"client-probe";
        const bool host_probe=argc==9 && std::wstring(argv[1])==L"host-probe";
        const bool join_probe=argc==9 && std::wstring(argv[1])==L"join-probe";
        const bool host_game=argc==9 && (std::wstring(argv[1])==L"host" || host_probe);
        const bool join_game=argc==9 && (std::wstring(argv[1])==L"join" || join_probe);
        const bool play_probe=argc==8 && std::wstring(argv[1])==L"play-probe";
        const bool play_game=argc==8 && (std::wstring(argv[1])==L"play" || play_probe);
        if (!play_game && !test_client && !host_game && !join_game && (argc != 5 || std::wstring(argv[1]) != L"probe")) {
            std::cerr << "Usage: AzerothCoop probe <isolated-game.exe> <isolated-profile> <module.dll>\n"
                         "       AzerothCoop status <pid>\n"
                         "       AzerothCoop attach <pid> <transport.dll>\n"
                         "       AzerothCoop transport <pid> status|probe|host|join [arguments]\n"
                         "       AzerothCoop play|play-probe <game.exe> <profile> <base.dll> <transport.dll> <gameplay.dll> <lobby.dll>\n"
                         "       AzerothCoop host|join|client-probe <game.exe> <profile> <base.dll> <transport.dll> <host> <port> <token>\n"; return 2;
        }
        const auto game = fs::canonical(argv[2]);
        const auto profile = fs::absolute(argv[3]).lexically_normal();
        const auto dll = fs::canonical(argv[4]);
        if (sha256(game) != azs::game_sha256) throw std::runtime_error("Unsupported executable SHA256; no game was started");
        require(fs::is_directory(game.parent_path() / "Data"), "Isolated Data directory is missing");
        fs::create_directories(profile / "AppData/Roaming");
        fs::create_directories(profile / "AppData/Local");
        fs::create_directories(profile / "benilla");
        auto env = environment_block(profile, game,play_game);
        auto command = L"\"" + game.wstring() + L"\"";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        if(test_client || host_probe || join_probe || play_probe) { startup.dwFlags=STARTF_USESHOWWINDOW; startup.wShowWindow=SW_SHOWMINNOACTIVE; }
        PROCESS_INFORMATION info{};
        require(CreateProcessW(game.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
            env.data(), profile.c_str(), &startup, &info), "Launching game with external mod profile");
        Handle process(info.hProcess), primary_thread(info.hThread);
        try {
            start_crash_watcher(info.dwProcessId);
            require(ResumeThread(primary_thread.value)!=static_cast<DWORD>(-1),"Resuming monitored game");
            inject(process.value, info.dwProcessId, dll);
            const auto ready_name = L"Local\\AzerothCoopReady-" + std::to_wstring(info.dwProcessId);
            Handle ready;
            for (int i = 0; i < 100; ++i) {
                ready.value = OpenEventW(SYNCHRONIZE, FALSE, ready_name.c_str());
                if (ready.value) break;
                Sleep(25);
            }
            require(ready.value && WaitForSingleObject(ready.value, 10000) == WAIT_OBJECT_0, "Module initialization");
            const auto state = query(info.dwProcessId);
            if (state.find("\"hook_installed\":true") == std::string::npos) throw std::runtime_error(state);
            if(play_game) {
                inject(process.value,info.dwProcessId,fs::canonical(argv[5]));
                const auto transport=query(info.dwProcessId,L"AzerothCoopTransport-");
                if(transport.find("\"failure\":\"\"")==std::string::npos) throw std::runtime_error(transport);
                inject(process.value,info.dwProcessId,fs::canonical(argv[6]));
                const auto gameplay=query(info.dwProcessId,L"AzerothCoopGameplay-");
                if(gameplay.find("\"hooks_installed\":true")==std::string::npos) throw std::runtime_error(gameplay);
                inject(process.value,info.dwProcessId,fs::canonical(argv[7]));
                const auto lobby=query(info.dwProcessId,L"AzerothCoopLobby-");
                if(lobby.find("\"hooks_installed\":true")==std::string::npos) throw std::runtime_error(lobby);
            }
            if(test_client || host_game || join_game) {
                inject(process.value,info.dwProcessId,fs::canonical(argv[5]));
                query(info.dwProcessId,L"AzerothCoopTransport-");
                std::string configuration=host_game ? "host " : "join ";
                for(int i=6;i<9;++i) {
                    if(i>6) configuration+=' ';
                    for(const wchar_t* p=argv[i];*p;++p) {
                        if(*p>127) throw std::runtime_error("Use an ASCII host address and room token");
                        configuration+=static_cast<char>(*p);
                    }
                }
                configuration+='\n';
                const auto bridge_state=query(info.dwProcessId,L"AzerothCoopTransport-",configuration);
                if(bridge_state.find("\"error\"")!=std::string::npos) throw std::runtime_error(bridge_state);
                if(test_client) {
                    // Exercise the same registered, detoured factory the original
                    // engine calls, with native stream queues in both processes.
                    std::string response;
                    for(int i=0;i<100;++i) {
                        response=query(info.dwProcessId,L"AzerothCoopTransport-","probe-redirected 8085\n");
                        if(response.find("server_not_ready")==std::string::npos) break;
                        Sleep(25);
                    }
                    std::ofstream report(profile/"client-transport-probe.json"); report<<response;
                    std::cout<<response;
                    bool ok=response.find("\"hex\":\"0006ec0100eeffc0\"")!=std::string::npos;
                    const auto challenge_size=GetEnvironmentVariableA("AZS_CLIENT_PROBE_AUTH_HEX",nullptr,0);
                    if(ok && challenge_size) {
                        if(challenge_size>8192) throw std::runtime_error("Probe challenge exceeds its size limit");
                        std::string challenge(challenge_size,'\0');
                        challenge.resize(GetEnvironmentVariableA("AZS_CLIENT_PROBE_AUTH_HEX",challenge.data(),challenge_size));
                        const auto auth=query(info.dwProcessId,L"AzerothCoopTransport-",std::string("probe-redirected 3724 ")+challenge+"\n");
                        std::ofstream auth_report(profile/"client-auth-probe.json"); auth_report<<auth;
                        std::cout<<auth;
                        ok=auth.find("\"bytes\":119")!=std::string::npos;
                    }
                    TerminateProcess(process.value,ok?0:1);
                    WaitForSingleObject(process.value,5000);
                    if(!ok) throw std::runtime_error("Original client factory did not receive the remote native challenge");
                    return 0;
                }
                std::cout<<bridge_state;
            }
            std::ofstream metadata(profile / "instance.json");
            metadata << "{\"pid\":" << info.dwProcessId << ",\"phase\":\"native-integration-probe\",\"coop_implemented\":false}\n";
            std::cout << state;
        } catch (...) {
            TerminateProcess(process.value, 1); // Only this fresh, isolated child.
            throw;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
