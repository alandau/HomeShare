#include "lib/win/window.h"
#include "lib/win/raii.h"
#include "SocketThread.h"
#include "resource.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>

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
    HWND m_hwndChild;
    SocketThreadApi socketThread_;
};

LRESULT RootWindow::OnCreate()
{
    socketThread_.Init(GetHWND());
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
        if (m_hwndChild) {
            SetWindowPos(m_hwndChild, NULL, 0, 0,
                GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam),
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;

    case WM_SETFOCUS:
        if (m_hwndChild) {
            SetFocus(m_hwndChild);
        }
        return 0;
    case WM_USER:
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
