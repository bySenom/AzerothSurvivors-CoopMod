#pragma once
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iomanip>

namespace azs {
inline std::filesystem::path crash_directory;
inline LPTOP_LEVEL_EXCEPTION_FILTER previous_crash_filter=nullptr;
inline LONG WINAPI log_native_crash(EXCEPTION_POINTERS* exception) {
    try {
        std::ofstream out(crash_directory/"native-crash.txt",std::ios::trunc);
        const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        out<<std::hex<<"exception=0x"<<exception->ExceptionRecord->ExceptionCode<<" game_base=0x"<<base<<'\n';
        CONTEXT context=*exception->ContextRecord;
        for(unsigned depth=0;depth<40 && context.Rsp;++depth) {
            out<<"frame "<<std::dec<<depth<<std::hex<<" rip=0x"<<context.Rip<<" rsp=0x"<<context.Rsp;
            if(context.Rip>=base && context.Rip-base<0x10000000) out<<" game_rva=0x"<<(context.Rip-base);
            out<<'\n';
            DWORD64 image=0;
            const auto entry=RtlLookupFunctionEntry(context.Rip,&image,nullptr);
            if(entry) {
                PVOID handler=nullptr;DWORD64 frame=0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER,image,context.Rip,entry,&context,&handler,&frame,nullptr);
            } else {
                SIZE_T read=0;DWORD64 next=0;
                if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(context.Rsp),&next,sizeof(next),&read)) break;
                context.Rip=next;context.Rsp+=8;
            }
            if(!context.Rip) break;
        }
    } catch(...) {}
    return previous_crash_filter?previous_crash_filter(exception):EXCEPTION_CONTINUE_SEARCH;
}
inline void install_crash_log(const std::filesystem::path& directory) {
    crash_directory=directory;previous_crash_filter=SetUnhandledExceptionFilter(&log_native_crash);
}
}
