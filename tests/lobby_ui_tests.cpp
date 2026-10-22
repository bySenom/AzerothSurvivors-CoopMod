// Exercises our own controls and offscreen paint code. No game input is sent.
#include "../src/lobby.cpp"
#include <fstream>
#include <iostream>
#include <filesystem>

void save_bitmap(Dialog& dialog,const std::filesystem::path& path) {
    RECT size{}; GetClientRect(dialog.window,&size);
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=size.right; info.bmiHeader.biHeight=size.bottom;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    void* pixels=nullptr;
    const auto dc=CreateCompatibleDC(nullptr);
    const auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if(!bitmap || !pixels) throw std::runtime_error("offscreen_bitmap_failed");
    const auto previous=SelectObject(dc,bitmap);
    SendMessageW(dialog.window,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_CHILDREN|PRF_NONCLIENT);
    // Include the edit's non-client border in the offscreen capture.
    if(dialog.input) {
        RECT rect{}; GetWindowRect(dialog.input,&rect);
        MapWindowPoints(nullptr,dialog.window,reinterpret_cast<POINT*>(&rect),2);
        const auto saved=SaveDC(dc); SetViewportOrgEx(dc,rect.left,rect.top,nullptr);
        SendMessageW(dialog.input,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_NONCLIENT|PRF_ERASEBKGND);
        RestoreDC(dc,saved);
    }
    BITMAPFILEHEADER header{}; header.bfType=0x4d42;
    header.bfOffBits=sizeof(header)+sizeof(info.bmiHeader);
    header.bfSize=header.bfOffBits+size.right*size.bottom*4;
    std::ofstream output(path,std::ios::binary);
    output.write(reinterpret_cast<const char*>(&header),sizeof(header));
    output.write(reinterpret_cast<const char*>(&info.bmiHeader),sizeof(info.bmiHeader));
    output.write(static_cast<const char*>(pixels),size.right*size.bottom*4);
    SelectObject(dc,previous); DeleteObject(bitmap); DeleteDC(dc);
}
void check_controls(Dialog& dialog) {
    RECT parent{}; GetClientRect(dialog.window,&parent);
    for(auto child:dialog.controls) {
        azs::lobby::require(GetParent(child)==dialog.window,"control_outside_ingame_root");
        RECT rect{}; GetWindowRect(child,&rect); MapWindowPoints(nullptr,dialog.window,reinterpret_cast<POINT*>(&rect),2);
        azs::lobby::require(rect.left>=0 && rect.top>=0 && rect.right<=parent.right && rect.bottom<=parent.bottom,"control_outside_client");
    }
}
template<class Predicate> void wait_for(Predicate predicate,const char* message) {
    const auto deadline=GetTickCount64()+4000;
    while(!predicate() && GetTickCount64()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    azs::lobby::require(predicate(),message);
}
int wmain(int argc,wchar_t** argv) {
    try {
        own_module=GetModuleHandleW(nullptr);
        const auto foreground=GetForegroundWindow();
        WNDCLASSW cls{}; cls.lpfnWndProc=DefWindowProcW; cls.hInstance=own_module; cls.lpszClassName=L"AZSLobbyTestParent";
        RegisterClassW(&cls);
        // An owned no-activate tool window stays outside the desktop. Standard
        // EDIT controls do not implement WM_PRINT while an ancestor is hidden.
        const auto parent=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,cls.lpszClassName,L"Lobby test",
            WS_POPUP|WS_VISIBLE,-30000,-30000,1280,720,nullptr,nullptr,own_module,nullptr);
        azs::lobby::require(parent!=nullptr,"test_parent_failed");
        const std::filesystem::path folder=argc>1?argv[1]:L"lobby-ui-results";
        std::filesystem::create_directories(folder);
        {
            Dialog dialog; dialog.create(parent); check_controls(dialog);
            save_bitmap(dialog,folder/L"mode.bmp");
            dialog.command(coop); azs::lobby::require(dialog.page==Page::coop,"coop_navigation_failed"); check_controls(dialog);
            save_bitmap(dialog,folder/L"coop.bmp");
            SetWindowTextW(dialog.input,L"invalid code"); dialog.command(join_lobby);
            azs::lobby::require(!dialog.message.empty() && dialog.page==Page::coop && !dialog.proceed,"invalid_code_started_game");
            session=std::make_unique<azs::lobby::Session>([](bool,const azs::lobby::Invite&,std::uint16_t){});
            session->host(27136,0,"127.0.0.1"); dialog.layout(Page::host); check_controls(dialog);
            azs::lobby::require(!IsWindowEnabled(GetDlgItem(dialog.window,start)),"empty_lobby_start_enabled");
            save_bitmap(dialog,folder/L"host.bmp");
            azs::lobby::Session peer([](bool,const azs::lobby::Invite&,std::uint16_t){});
            peer.join(session->state().code);
            wait_for([&]{return peer.state().connected && session->state().connected;},"ui_test_peer_not_connected");
            peer.ready(true); wait_for([&]{return session->state().ready;},"ui_test_ready_not_received");
            dialog.tick();
            azs::lobby::require(IsWindowEnabled(GetDlgItem(dialog.window,start)),"ready_lobby_start_disabled");
            save_bitmap(dialog,folder/L"host-ready.bmp");
            peer.stop(); wait_for([&]{return !session->state().connected;},"ui_test_peer_not_left");
            dialog.tick();
            azs::lobby::require(!IsWindowEnabled(GetDlgItem(dialog.window,start)),"disconnected_lobby_start_enabled");
            dialog.command(join_lobby);
            azs::lobby::require(dialog.page==Page::host && session->state().hosting,"host_code_enter_replaced_lobby");
            SetWindowPos(parent,nullptr,0,0,1500,900,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            dialog.tick(); check_controls(dialog);
            RECT overlay{}; GetClientRect(dialog.window,&overlay);
            azs::lobby::require(overlay.right==1500 && overlay.bottom==900,"resized_modal_did_not_cover_client");
            dialog.command(back); azs::lobby::require(dialog.page==Page::coop,"leave_lobby_navigation_failed");
            dialog.command(back); azs::lobby::require(dialog.page==Page::mode,"back_to_mode_failed");
            dialog.command(solo); azs::lobby::require(dialog.done && dialog.proceed,"solo_did_not_approve_native_pick");
        }
        {
            azs::lobby::Session host([](bool,const azs::lobby::Invite&,std::uint16_t){});
            host.host(27136,0,"127.0.0.1");
            session=std::make_unique<azs::lobby::Session>([](bool,const azs::lobby::Invite&,std::uint16_t){});
            session->join(host.state().code);
            Dialog dialog; dialog.create(parent); dialog.layout(Page::guest);
            wait_for([&]{return host.state().connected && session->state().connected;},"guest_ui_not_connected");
            dialog.tick(); check_controls(dialog); save_bitmap(dialog,folder/L"guest.bmp");
            azs::lobby::require(IsWindowEnabled(GetDlgItem(dialog.window,ready)),"connected_guest_ready_disabled");
            dialog.command(ready); wait_for([&]{return host.state().ready;},"guest_button_did_not_send_ready");
            host.start(); wait_for([&]{return session->state().started;},"guest_did_not_receive_start");
            dialog.tick(); azs::lobby::require(dialog.done && dialog.proceed,"host_start_did_not_approve_guest_pick");
        }
        session.reset(); DestroyWindow(parent);
        azs::lobby::require(GetForegroundWindow()==foreground,"test_changed_foreground_window");
        std::cout<<"Native child controls, modal resize, Solo/Coop navigation, invalid-code rejection, ready/disconnect gates and guest start approval: passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
}
