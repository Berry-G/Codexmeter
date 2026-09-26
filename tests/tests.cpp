#define CODEXMETER_TESTING
#include "../src/main.cpp"
#include <iostream>
#include <fstream>
#include <psapi.h>
#pragma comment(lib,"psapi.lib")
static int checks = 0;
static void check(bool condition,const char* name) {
    ++checks;
    if (!condition) { std::cerr << "FAIL: " << name << "\n"; std::exit(1); }
}
static Json windowValue(int used,int mins,long long reset = 2000000000) {
    return {{"usedPercent",used},{"windowDurationMins",mins},{"resetsAt",reset}};
}
static void quotaTests() {
    auto both = meter::parseQuota({{"rateLimits",{{"primary",windowValue(30,300)},{"secondary",windowValue(60,10080)}}}});
    check(both.shortTerm.remaining == 70 && both.weekly.remaining == 40,"remaining percentages");
    auto weeklyOnly = meter::parseQuota({{"rateLimits",{{"primary",windowValue(84,10080)},{"secondary",nullptr}}}});
    check(!weeklyOnly.shortTerm.known && weeklyOnly.weekly.remaining == 16,"weekly primary is not mislabeled short-term");
    auto multi = meter::parseQuota({{"rateLimits",{{"primary",windowValue(1,300)}}},{"rateLimitsByLimitId",{{"codex",{{"primary",windowValue(90,300)}}},{"other",{{"primary",windowValue(20,300)}}}}}});
    check(multi.shortTerm.remaining == 10,"multi-bucket preferred over legacy");
    bool threw = false;
    try { meter::parseQuota({{"rateLimitsByLimitId",{{"other",{{"primary",windowValue(20,300)}}}}}}); } catch (...) { threw = true; }
    check(threw,"never substitute another bucket");
    check(!meter::parseWindow(nullptr).known,"null is unavailable");
    check(!meter::parseWindow({{"usedPercent","70"}}).known,"invalid values unavailable");
    check(meter::parseWindow(windowValue(150,300)).remaining == 0,"clamp overrun");
    check(meter::parseWindow(windowValue(-2,300)).remaining == 100,"clamp negative");
    auto unknownDuration = meter::parseQuota({{"rateLimits",{{"primary",{{"usedPercent",20},{"windowDurationMins",nullptr}}}}}});
    check(!unknownDuration.shortTerm.known && !unknownDuration.weekly.known,"unknown duration not invented");
    both.shortTerm.reset = 100; both.weekly.reset = 200;
    check(meter::nextReset(both) == 100,"earliest reset");
    both.shortTerm.reset = 0; check(meter::nextReset(both) == 200,"missing reset");
    check(countdown(0) == L"정보 없음","unknown reset text");
    check(countdown(1) == L"갱신 대기","expired reset not fake zero");
    check(meter::remainingTime(3*86400+13*3600+40*60) == L"3d 13h","over 24 hours shows days and hours only");
    check(meter::remainingTime(20*3600+15*60) == L"20h 15m","under 24 hours shows hours and minutes");
    check(meter::remainingTime(86400) == L"24h 0m","exactly 24 hours uses hours and minutes");
    check(meter::remainingTime(86401) == L"1d 0h","one second over 24 hours uses days and hours");
    check(meter::remainingTime(1) == L"0h 1m","positive fractional minutes round up");
}
static void layoutTests() {
    auto p = meter::placeTitlebar(100,100,862,0,32,96,280,180,8,360);
    check(p.x == 674 && p.y == 104 && p.width == 280 && p.height == 24,"fixed immediately left of caption buttons");
    auto moved = meter::placeTitlebar(-900,250,862,0,32,96,280,180,8,360);
    check(moved.x == p.x-1000 && moved.y == p.y+150,"negative-coordinate monitor movement");
    auto small = meter::placeTitlebar(0,0,588,0,32,96,280,180,8,360);
    check(small.compact && small.width == 220 && small.height == 24 && small.x == 360,"narrow title-bar layout preserves menu area");
    auto largeDpi = meter::placeTitlebar(0,0,1293,0,48,144,280,180,8,360);
    check(largeDpi.width == 420 && largeDpi.height == 36 && largeDpi.x == 861 && largeDpi.y == 6,"150 percent DPI");
    check(meter::placeTitlebar(0,0,540,0,32,96,280,180,8,360).width == 0,"hide instead of covering menu on insufficient width");
    check(meter::placeTitlebar(0,0,547,0,32,96,280,180,8,360).width == 0,"one pixel under minimum hides");
    check(meter::placeTitlebar(0,0,548,0,32,96,280,180,8,360).width == 180,"exact minimum width fits");
    check(meter::placeTitlebar(0,0,862,0,24,96,280,180,8,360).width == 0,"insufficient caption height hides");
    check(p.x+p.width == 100+862-8,"minimize button gap is constant");
    meter::Quota two; two.shortTerm.known = true; two.weekly.known = true;
    check(meter::stripSize(two,280,180).desired == 280,"two quotas use full strip");
    two.shortTerm.known = false;
    check(meter::stripSize(two,280,180).desired == 180 && meter::stripSize(two,280,180).minimum == 110,"missing short-term quota shrinks strip");
    check(!meter::shouldShow(false,true,false,false,false,1000,800),"other application hidden");
    check(!meter::shouldShow(true,true,true,false,false,1000,800),"minimized hidden");
    check(!meter::shouldShow(true,true,false,true,false,1000,800),"virtual desktop cloak hidden");
    check(!meter::shouldShow(true,true,false,false,true,1000,800),"paused hidden");
    check(!meter::shouldShow(true,false,false,false,false,1000,800),"invisible host hidden");
    check(!trustedPath(L"C:\\Other\\ChatGPT.exe"),"title/name alone not trusted");
    check(trustedPath(L"C:\\Program Files\\WindowsApps\\OpenAI.Codex_1_x64__x\\app\\ChatGPT.exe"),"Store Codex package trusted");
    check(!trustedPath(L"C:\\Program Files\\WindowsApps\\OpenAI.ChatGPT_1_x64__x\\app\\ChatGPT.exe"),"ChatGPT package not Codex");
}
static void savePreview(const wchar_t* path,int width,int height,bool narrow,bool weeklyOnly = false,bool light = false) {
    currentDpi = 96; compact = narrow; darkTheme = !light;
    quota.shortTerm = meter::parseWindow(windowValue(30,300)); quota.weekly = meter::parseWindow(windowValue(60,10080));
    if (weeklyOnly) quota.shortTerm = {};
    quota.shortTerm.reset = static_cast<long long>(std::time(nullptr))+8100; status.clear(); stale = false;
    quota.weekly.reset = static_cast<long long>(std::time(nullptr))+3*86400+13*3600;
    BITMAPINFO info = {}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr; HDC dc = CreateCompatibleDC(nullptr); HBITMAP bmp = CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    auto old = SelectObject(dc,bmp); paintContent(dc,{0,0,width,height}); GdiFlush();
    // Composite premultiplied per-pixel alpha onto a light/dark sample title bar.
    DWORD* data = static_cast<DWORD*>(pixels);
    DWORD backdrop = light ? 0xf0edf3 : 0x20242b;
    int zeroAlpha = 0, fractionalAlpha = 0;
    for (int i=0;i<width*height;i++) {
        unsigned alpha = data[i]>>24;
        if (!alpha) ++zeroAlpha;
        else if (alpha < 255) ++fractionalAlpha;
        DWORD rgb = 0;
        for (int shift : {0,8,16}) rgb |= std::min<DWORD>(255,((data[i]>>shift)&255)+(((backdrop>>shift)&255)*(255-alpha)+127)/255)<<shift;
        data[i] = rgb;
    }
    check(zeroAlpha > width*height/2,"renderer leaves title bar background transparent");
    check(fractionalAlpha > 0,"renderer uses smooth per-pixel glyph alpha");
    BITMAPFILEHEADER header = {}; header.bfType = 0x4d42; header.bfOffBits = sizeof(header)+sizeof(info.bmiHeader); header.bfSize = header.bfOffBits+width*height*4;
    HANDLE file = CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr); DWORD bytes;
    WriteFile(file,&header,sizeof(header),&bytes,nullptr); WriteFile(file,&info.bmiHeader,sizeof(info.bmiHeader),&bytes,nullptr); WriteFile(file,pixels,width*height*4,&bytes,nullptr); CloseHandle(file);
    SelectObject(dc,old); DeleteObject(bmp); DeleteDC(dc);
}
static void pump() { MSG msg; while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
static void focusTestWindow(HWND window) {
    DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(),nullptr);
    DWORD thisThread = GetCurrentThreadId();
    bool attached = foregroundThread != thisThread && AttachThreadInput(thisThread,foregroundThread,TRUE);
    BringWindowToTop(window); SetForegroundWindow(window);
    if (attached) AttachThreadInput(thisThread,foregroundThread,FALSE);
}
static void nativeTests() {
    HWND originalForeground = GetForegroundWindow();
    targetOverride = exePath(); lastPid = 0;
    quota.shortTerm.known = true; quota.weekly.known = true;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW hostClass = {}; hostClass.hInstance = instance; hostClass.lpfnWndProc = DefWindowProcW; hostClass.lpszClassName = L"CodexmeterTestHost"; RegisterClassW(&hostClass);
    HWND host = CreateWindowW(hostClass.lpszClassName,L"Codexmeter test host",WS_OVERLAPPEDWINDOW,100,100,1000,700,nullptr,nullptr,instance,nullptr);
    WNDCLASSW cls = {}; cls.hInstance = instance; cls.lpfnWndProc = windowProc; cls.lpszClassName = L"CodexmeterTestOverlay"; RegisterClassW(&cls);
    overlay = CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_LAYERED|WS_EX_TRANSPARENT,cls.lpszClassName,L"Codexmeter test",WS_POPUP,0,0,660,54,nullptr,nullptr,instance,nullptr);
    ShowWindow(host,SW_SHOW); focusTestWindow(host); pump();
    check(GetForegroundWindow() == host,"native test focus precondition"); track();
    check(GetForegroundWindow() == host,"overlay does not steal foreground");
    check(IsWindowVisible(overlay),"overlay visible on matching foreground");
    check((GetWindowLongPtrW(overlay,GWL_EXSTYLE)&WS_EX_TOPMOST) != 0,"overlay above host");
    check((GetWindowLongPtrW(overlay,GWL_EXSTYLE)&WS_EX_TRANSPARENT) != 0,"click-through layered style");
    RECT before; GetWindowRect(overlay,&before);
    SetWindowPos(host,nullptr,300,220,1000,700,SWP_NOZORDER); pump(); track();
    RECT after; GetWindowRect(overlay,&after);
    check(after.left-before.left == 200 && after.top-before.top == 120,"native window move follows exactly");
    SetWindowPos(host,nullptr,0,0,px(730),px(500),SWP_NOMOVE|SWP_NOZORDER); pump(); track(); GetWindowRect(overlay,&after);
    check(IsWindowVisible(overlay) && compact && after.right-after.left < px(280),"native resize changes compact layout");
    SetWindowPos(host,nullptr,0,0,420,500,SWP_NOMOVE|SWP_NOZORDER); pump(); track();
    check(!IsWindowVisible(overlay),"native insufficient title-bar space hides");
    SetWindowPos(host,nullptr,0,0,1000,700,SWP_NOMOVE|SWP_NOZORDER); pump(); track();
    check(IsWindowVisible(overlay),"native grow restores title-bar strip");
    paused = true; track(); check(!IsWindowVisible(overlay),"native pause hides"); paused = false;
    ShowWindow(host,SW_MINIMIZE); pump(); track(); check(!IsWindowVisible(overlay),"native minimize hides");
    ShowWindow(host,SW_RESTORE); focusTestWindow(host); pump(); track(); check(IsWindowVisible(overlay),"native restore shows");
    targetOverride = L"C:\\unrelated.exe"; lastPid = 0; track();
    check(!IsWindowVisible(overlay),"native unmatched application hidden");
    check((GetWindowLongPtrW(overlay,GWL_EXSTYLE)&WS_EX_TOPMOST) == 0,"topmost removed while hidden");
    DestroyWindow(host); pump();
    check(IsWindow(overlay),"host destruction does not destroy overlay");
    DestroyWindow(overlay); pump();
    if (IsWindow(originalForeground)) focusTestWindow(originalForeground);
}
int main(int argc,char** argv) {
    using DpiFn = BOOL (WINAPI*)(HANDLE);
    auto setDpi = reinterpret_cast<DpiFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetProcessDpiAwarenessContext"));
    if (setDpi) setDpi(reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-4)));
    stopEvent = CreateEventW(nullptr,TRUE,FALSE,nullptr); refreshEvent = CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if (argc > 1 && std::string(argv[1]) == "--stop") {
        HWND live = FindWindowW(L"CodexmeterOverlay",nullptr);
        if (live) PostMessageW(live,WM_CLOSE,0,0);
    } else if (argc > 1 && std::string(argv[1]) == "--inspect") {
        HWND live = FindWindowW(L"CodexmeterOverlay",nullptr);
        check(live != nullptr,"live overlay process found");
        RECT r; GetWindowRect(live,&r);
        HWND fg = GetAncestor(GetForegroundWindow(),GA_ROOTOWNER);
        DWORD pid; GetWindowThreadProcessId(fg,&pid);
        bool matching = trustedPath(imagePath(pid));
        if (matching) {
            RECT frame = {}, buttons = {}; GetWindowRect(fg,&frame);
            bool dwmOK = dwmAttribute(fg,5,&buttons,sizeof(buttons));
            RECT client = {}; GetClientRect(fg,&client); POINT origin = {}; ClientToScreen(fg,&origin);
            std::cout << "DPI=" << dpiFor(fg) << ", frame=" << frame.left << ',' << frame.top << ',' << frame.right << ',' << frame.bottom
                      << ", DWM caption OK=" << dwmOK << ", buttons=" << buttons.left << ',' << buttons.top << ',' << buttons.right << ',' << buttons.bottom
                      << ", client=" << origin.x << ',' << origin.y << ',' << client.right << ',' << client.bottom << "\n";
        }
        std::cout << "Foreground matches Codex=" << matching << ", overlay visible=" << IsWindowVisible(live)
                  << ", rect=" << r.left << ',' << r.top << ',' << r.right << ',' << r.bottom << "\n";
        auto expected = matching ? titlebarPlacement(fg,dpiFor(fg)) : meter::Placement{};
        if (!matching || !expected.width) check(!IsWindowVisible(live),"real overlay hidden outside Codex or with no caption space");
        if (matching && IsWindowVisible(live)) {
            check(r.right == expected.x+expected.width && r.top == expected.y && r.bottom-r.top == expected.height,"real strip anchored before minimize button");
        }
        check(GetWindowLongPtrW(live,GWL_EXSTYLE)&WS_EX_LAYERED,"real overlay uses per-pixel layered surface");
        check(GetWindowLongPtrW(live,GWL_EXSTYLE)&WS_EX_NOACTIVATE,"live no-activate flag");
        check(GetWindowLongPtrW(live,GWL_EXSTYLE)&WS_EX_TRANSPARENT,"live click-through flag");
    } else if (argc > 1 && std::string(argv[1]) == "--live") {
        configPath = directory(exePath())+L"\\Codexmeter.ini"; codexPath = discoverCodex();
        Update update = fetch(); check(update.ok,"native App Server live read");
        check(update.value.shortTerm.known || update.value.weekly.known,"at least one real quota window");
        std::cout << "Live read completed. Account quota values are not printed.\n";
    } else {
        quotaTests(); layoutTests();
        savePreview(L"dist\\preview-wide.bmp",280,24,false); savePreview(L"dist\\preview-narrow.bmp",180,24,true);
        savePreview(L"dist\\preview-weekly-light.bmp",180,24,false,true,true);
        savePreview(L"dist\\preview-weekly-dark.bmp",180,24,false,true,false);
        if (argc > 1 && std::string(argv[1]) == "--native") nativeTests();
    }
    CloseHandle(stopEvent); CloseHandle(refreshEvent);
    std::cout << checks << " checks passed.\n"; return 0;
}
