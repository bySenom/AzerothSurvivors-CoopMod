#pragma once
#include <windows.h>
#include <filesystem>
#include <cstdint>
#include <cstdio>

namespace azs {
inline LPTOP_LEVEL_EXCEPTION_FILTER previous_crash_filter=nullptr;
inline HANDLE crash_file=INVALID_HANDLE_VALUE, crash_requested=nullptr, crash_finished=nullptr;
inline LONG crash_entered=0;
inline LONG WINAPI log_native_crash(EXCEPTION_POINTERS* exception) {
    if(InterlockedCompareExchange(&crash_entered,1,0)==0 && exception && exception->ExceptionRecord && exception->ContextRecord) {
        // No streams, heap allocation, locks or stack unwinding in the faulting
        // thread. The independent watcher writes the dump while this context lives.
        const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        SYSTEMTIME utc{}; GetSystemTime(&utc);
        char text[1024]{};
        const int length=_snprintf_s(text,sizeof(text),_TRUNCATE,
            "utc=%04u-%02u-%02uT%02u:%02u:%02uZ\r\npid=%lu\r\nthread=%lu\r\nexception=0x%08lx\r\nexception_pointers=0x%llx\r\ngame_base=0x%llx\r\nrip=0x%llx\r\nrsp=0x%llx\r\ngame_rva=0x%llx\r\n",
            utc.wYear,utc.wMonth,utc.wDay,utc.wHour,utc.wMinute,utc.wSecond,
            GetCurrentProcessId(),GetCurrentThreadId(),exception->ExceptionRecord->ExceptionCode,
            reinterpret_cast<std::uintptr_t>(exception),base,exception->ContextRecord->Rip,exception->ContextRecord->Rsp,
            exception->ContextRecord->Rip>=base?exception->ContextRecord->Rip-base:0);
        if(crash_file!=INVALID_HANDLE_VALUE && length>0) {
            DWORD written=0; WriteFile(crash_file,text,static_cast<DWORD>(length),&written,nullptr); FlushFileBuffers(crash_file);
        }
        if(crash_requested && crash_finished) {
            SetEvent(crash_requested);
            // A missing or failed helper must never keep a crashed game alive.
            WaitForSingleObject(crash_finished,8000);
        }
    }
    return previous_crash_filter?previous_crash_filter(exception):EXCEPTION_CONTINUE_SEARCH;
}
inline void install_crash_log(const std::filesystem::path& directory) {
    // Session directories are unique. Direct native CLI users also retain all
    // runs instead of overwriting the historical profile/native-crash.txt.
    wchar_t configured[32768]{};
    const auto count=GetEnvironmentVariableW(L"AZS_COOP_CRASH_DIR",configured,32768);
    FILETIME now{}; GetSystemTimeAsFileTime(&now);
    const auto stamp=(static_cast<std::uint64_t>(now.dwHighDateTime)<<32)|now.dwLowDateTime;
    auto target=directory/"crashes"/(std::to_wstring(stamp)+L"-"+std::to_wstring(GetCurrentProcessId()));
    if(count && count<32768) target=configured;
    std::error_code error; std::filesystem::create_directories(target,error);
    crash_file=CreateFileW((target/"native-crash.txt").c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    const auto suffix=std::to_wstring(GetCurrentProcessId());
    crash_requested=OpenEventW(EVENT_MODIFY_STATE,FALSE,(L"Local\\AzerothCoopCrashRequested-"+suffix).c_str());
    crash_finished=OpenEventW(SYNCHRONIZE,FALSE,(L"Local\\AzerothCoopCrashFinished-"+suffix).c_str());
    previous_crash_filter=SetUnhandledExceptionFilter(&log_native_crash);
}
}
