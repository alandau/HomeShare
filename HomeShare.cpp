#include "lib/win/window.h"
#include "lib/win/raii.h"
#include "SocketThread.h"
#include "Logger.h"
#include "resource.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>

class ListViewLogger : public Logger {
public:
    ListViewLogger(Window* window, HWND listView)
        : window_(window)
        , listView_(listView)
    {}
protected:
    void logString(LogLevel level, const std::wstring& s) override {
        window_->RunInThread([this, level, s] {
            logStringImpl(level, s);
        });
    }

    void logStringImpl(LogLevel level, const std::wstring& s) {
        std::time_t t = std::time(nullptr);
        struct tm tm;
        localtime_s(&tm, &t);
        std::wstring timeStr = fmt::format(L"{}-{:02}-{:02} {:02}:{:02}:{:02}",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec);
        auto levelToStr = [](LogLevel level) {
            switch (level) {
            case F: return L"Fatal";
            case E: return L"Error";
            case W: return L"Warning";
            case I: return L"Info";
            case D: return L"Debug";
            default: return L"Unknown";
            }
        };
        std::wstring levelStr = levelToStr(level);

        LVITEM item;
        item.mask = LVIF_TEXT;
        item.iItem = INT_MAX;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(levelStr.c_str());
        item.iItem = ListView_InsertItem(listView_, &item);
        if (item.iItem >= 0) {
            item.iSubItem = 1;
            item.pszText = const_cast<LPWSTR>(timeStr.c_str());
            ListView_SetItem(listView_, &item);
            item.iSubItem = 2;
            item.pszText = const_cast<LPWSTR>(s.c_str());
            ListView_SetItem(listView_, &item);
        }
    }
private:
    Window* window_;
    HWND listView_;
};

class RootWindow : public Window
{
public:
    LPCTSTR ClassName() override { return TEXT("HomeShare"); }
    void PaintContent(PAINTSTRUCT* pps) override;
    static RootWindow *Create();
protected:
    LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT OnCreate();
private:
    HWND logView_;
    std::unique_ptr<ListViewLogger> logger_;
    SocketThreadApi socketThread_;
};

LRESULT RootWindow::OnCreate()
{
    logView_ = CreateWindow(WC_LISTVIEW, NULL,
        WS_VISIBLE | WS_CHILD | WS_TABSTOP | LVS_NOSORTHEADER | LVS_SINGLESEL | LVS_REPORT,
        0, 0, 0, 0,
        GetHWND(),
        (HMENU)IDC_LOGVIEW,
        g_hinst,
        NULL);

    if (!logView_) return -1;

    ListView_SetExtendedListViewStyleEx(logView_,
        LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);

    LVCOLUMN lvc;

    lvc.mask = LVCF_TEXT | LVCF_WIDTH;
    lvc.cx = 200;
    lvc.pszText = TEXT("Level");
    ListView_InsertColumn(logView_, 0, &lvc);

    lvc.mask = LVCF_TEXT | LVCF_WIDTH;
    lvc.cx = 200;
    lvc.pszText = TEXT("Time");
    ListView_InsertColumn(logView_, 1, &lvc);

    lvc.mask = LVCF_TEXT | LVCF_WIDTH;
    lvc.cx = 800;
    lvc.pszText = TEXT("Message");
    ListView_InsertColumn(logView_, 2, &lvc);

    logger_.reset(new ListViewLogger(this, logView_));
    socketThread_.Init(logger_.get(), GetHWND());
    return 0;
}

wchar_t *buf;
int size;
LRESULT RootWindow::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_CREATE:
        return OnCreate();

    case WM_NCDESTROY:
        // Death of the root window ends the thread
        PostQuitMessage(0);
        break;

    case WM_SIZE:
        if (logView_) {
            int cx = GET_X_LPARAM(lParam);
            int cy = GET_Y_LPARAM(lParam);
            SetWindowPos(logView_, NULL, 0, cy / 2, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;

    case WM_SETFOCUS:
        if (logView_) {
            SetFocus(logView_);
        }
        return 0;
    case WM_USER+1:
        if (buf) {
            delete[] buf;
        }
        buf = (wchar_t*)wParam;
        size = lParam;
        InvalidateRect(GetHWND(), NULL, TRUE);
        return 0;
    case WM_COMMAND:
        switch (GET_WM_COMMAND_ID(wParam, lParam)) {
        case ID_FILE_SENDFILE:
            wchar_t filename[MAX_PATH];
            filename[0] = L'\0';
            OPENFILENAME ofn = { 0 };
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = GetHWND();
            ofn.lpstrFilter = L"All Files (*.*)\0*.*\0";
            ofn.lpstrFile = filename;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = L"Select file to send";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

            if (GetOpenFileName(&ofn)) {
                Contact c;
                c.hostname = "127.0.0.1";
                c.port = 8890;
                socketThread_.SendFile(c, filename);
            }
            break;
        }
        return 0;
    }

    return Window::HandleMessage(uMsg, wParam, lParam);
}

RootWindow *RootWindow::Create()
{
    RootWindow *self = new RootWindow();
    if (self->WinCreateWindow(0,
            TEXT("Scratch"), WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            NULL, LoadMenu(g_hinst, MAKEINTRESOURCE(IDR_MENU1)))) {
        return self;
    }
    delete self;
    return nullptr;
}

void RootWindow::PaintContent(PAINTSTRUCT* pps) {
    RECT rect;
    GetClientRect(GetHWND(), &rect);
    if (buf != nullptr) {
        DrawText(pps->hdc, buf, size, &rect, 0);
    }
}

void SetDpiAware() {
    HMODULE hLib = LoadLibrary(L"user32");
    if (!hLib) {
        return;
    }
    using SetProcessDPIAware = BOOL (WINAPI *)(void);
    SetProcessDPIAware pSetProcessDPIAware =
        (SetProcessDPIAware)GetProcAddress(hLib, "SetProcessDPIAware");
    if (pSetProcessDPIAware) {
        pSetProcessDPIAware();
    }
    FreeLibrary(hLib);
}

int WINAPI
WinMain(HINSTANCE hinst, HINSTANCE, LPSTR, int nShowCmd)
{
    SetDpiAware();

    g_hinst = hinst;
    ComInit comInit;
    InitCommonControls();

    RootWindow* w = RootWindow::Create();
    if (!w) {
        MessageBox(NULL, L"Can't create main window", NULL, MB_OK);
        return 1;
    }
    ShowWindow(w->GetHWND(), nShowCmd);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
