#include "lib/win/window.h"
#include "lib/win/raii.h"
#include "lib/win/encoding.h"
#include "SocketThread.h"
#include "DiskThread.h"
#include "Logger.h"
#include "resource.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>

std::string g_remoteIp;

static std::wstring GetDesktopPath() {
    wchar_t path[MAX_PATH];
    if (FAILED(SHGetFolderPath(NULL, CSIDL_DESKTOPDIRECTORY, NULL, SHGFP_TYPE_CURRENT, path))) {
        return L"";
    }
    return path;
}


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
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        int ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
        struct tm tm;
        localtime_s(&tm, &t);
        std::wstring timeStr = fmt::format(L"{}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
        auto levelToStr = [](LogLevel level) {
            switch (level) {
            case E: return L"Error";
            case W: return L"Warning";
            case I: return L"Info";
            case D: return L"Debug";
            default: return L"Unknown";
            }
        };
        std::wstring levelStr = levelToStr(level);

        LVITEM item;
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = INT_MAX;
        item.iSubItem = 0;
        item.iImage = level == D ? -1 : (int)level;
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

struct ContactData {
    enum class ConnectState {
        Disconnected, Connecting, Connected
    };
    Contact c;
    std::wstring displayName;
    std::string host;
    uint16_t port;
    ConnectState connectState = ConnectState::Disconnected;
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
    LRESULT OnNotify(NMHDR* pnm);
    void OnGetDispInfo(NMLVDISPINFO* pnmv);
private:
    HWND logView_;
    HWND contactView_;
    std::vector<ContactData> contactData_;
    std::unique_ptr<ListViewLogger> logger_;
    std::unique_ptr<SocketThreadApi> socketThread_;
    std::unique_ptr<DiskThread> diskThread_;

    void SelectAndSendFile(const ContactData& contactData);
};

LRESULT RootWindow::OnCreate()
{
    logView_ = CreateWindow(WC_LISTVIEW, NULL,
        WS_VISIBLE | WS_CHILD | WS_BORDER | WS_TABSTOP | LVS_NOSORTHEADER | LVS_SINGLESEL | LVS_REPORT,
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
    lvc.cx = 100;
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

    HIMAGELIST icons = ImageList_Create(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        ILC_MASK | ILC_COLOR32, 3, 0);
    ImageList_SetBkColor(icons, RGB(255, 255, 255));
    ImageList_AddIcon(icons, LoadIcon(NULL, IDI_ERROR));
    ImageList_AddIcon(icons, LoadIcon(NULL, IDI_WARNING));
    ImageList_AddIcon(icons, LoadIcon(NULL, IDI_INFORMATION));
    ListView_SetImageList(logView_, icons, LVSIL_SMALL);

    contactData_.push_back({ {"pubkey"}, L"Me" , "127.0.0.1", 8890});
    if (!g_remoteIp.empty()) {
        contactData_.push_back({ { "pubkey2" }, L"Other" , g_remoteIp, 8890 });
    }

    contactView_ = CreateWindow(WC_LISTVIEW, NULL,
        WS_VISIBLE | WS_CHILD | WS_BORDER | WS_TABSTOP | LVS_NOSORTHEADER | LVS_OWNERDATA | LVS_SINGLESEL | LVS_REPORT,
        0, 0, 0, 0,
        GetHWND(),
        (HMENU)IDC_CONTACTVIEW,
        g_hinst,
        NULL);

    if (!contactView_) return -1;

    ListView_SetExtendedListViewStyleEx(contactView_,
        LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);

    lvc.mask = LVCF_TEXT | LVCF_WIDTH;
    lvc.cx = 300;
    lvc.pszText = TEXT("Contact");
    ListView_InsertColumn(contactView_, 0, &lvc);
    
    lvc.mask = LVCF_TEXT | LVCF_WIDTH;
    lvc.cx = 150;
    lvc.pszText = TEXT("State");
    ListView_InsertColumn(contactView_, 1, &lvc);

    ListView_SetItemCount(contactView_, contactData_.size());

    logger_.reset(new ListViewLogger(this, logView_));
    socketThread_.reset(new SocketThreadApi);
    socketThread_->Init(logger_.get(), GetHWND());
    socketThread_->setOnConnectCb([this](const Contact& c, bool connected) {
        RunInThread([this, c, connected] {
            int index = -1;
            for (int i = 0; i < (int)contactData_.size(); i++) {
                if (contactData_[i].c == c) {
                    index = i;
                    break;
                }
            }
            if (index == -1) {
                // Can happen if the remote end which is not a contact connected/disconnected to us
                return;
            }
            contactData_[index].connectState = connected
                ? ContactData::ConnectState::Connected
                : ContactData::ConnectState::Disconnected;
            ListView_RedrawItems(contactView_, index, index);
            UpdateWindow(contactView_);
        });
    });
    diskThread_.reset(new DiskThread(logger_.get(), socketThread_.get(), GetDesktopPath()));
    return 0;
}

LRESULT RootWindow::OnNotify(NMHDR *pnm) {
    if (pnm->hwndFrom != contactView_) {
        return 0;
    }
    switch (pnm->code) {
    case LVN_GETDISPINFO:
        OnGetDispInfo(CONTAINING_RECORD(pnm, NMLVDISPINFO, hdr));
        break;
    case NM_RCLICK: {
        NMITEMACTIVATE* nma = CONTAINING_RECORD(pnm, NMITEMACTIVATE, hdr);
        if (nma->iItem == -1) {
            break;
        }
        ContactData& data = contactData_[nma->iItem];
        bool conn = data.connectState == ContactData::ConnectState::Connected;
        HMENU hMenu = CreatePopupMenu();
        AppendMenu(hMenu, MF_STRING | (conn ? MF_GRAYED : 0), 1, L"Connect");
        AppendMenu(hMenu, MF_STRING | (!conn ? MF_GRAYED : 0), 2, L"Send File");
        POINT p;
        GetCursorPos(&p);
        int item = TrackPopupMenu(hMenu, TPM_RETURNCMD, p.x, p.y, 0, GetHWND(), NULL);
        switch (item) {
        case 1:
            data.connectState = ContactData::ConnectState::Connecting;
            socketThread_->Connect(data.c, data.host, data.port);
            ListView_RedrawItems(contactView_, nma->iItem, nma->iItem);
            UpdateWindow(contactView_);
            break;
        case 2:
            SelectAndSendFile(data);
            break;
        }
        break;
    }
    }
    return 0;
}

void RootWindow::OnGetDispInfo(NMLVDISPINFO* pnmv) {
    size_t index = (size_t)pnmv->item.iItem;
    if (index >= contactData_.size()) {
        return;
    }
    ContactData& data = contactData_[index];

    if (pnmv->item.mask & LVIF_TEXT) {
        switch (pnmv->item.iSubItem) {
        case 0: 
            pnmv->item.pszText = const_cast<LPWSTR>(data.displayName.c_str());
            break;
        case 1:
            pnmv->item.pszText =
                data.connectState == ContactData::ConnectState::Connected ? L"Connected" :
                data.connectState == ContactData::ConnectState::Disconnected ? L"Disconnected" :
                data.connectState == ContactData::ConnectState::Connecting ? L"Connecting" : L"";
        }
    }
    
    if (pnmv->item.mask & LVIF_IMAGE) {
        pnmv->item.iImage = -1;
    }

    if (pnmv->item.mask & LVIF_STATE) {
        pnmv->item.state = 0;
    }
}

LRESULT RootWindow::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_CREATE:
        return OnCreate();

    case WM_NCDESTROY:
        // Death of the root window ends the thread
        PostQuitMessage(0);
        break;

    case WM_SIZE: {
        int cx = GET_X_LPARAM(lParam);
        int cy = GET_Y_LPARAM(lParam);
        if (contactView_) {
            SetWindowPos(contactView_, NULL, 0, 0, cx, cy / 2, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (logView_) {
            SetWindowPos(logView_, NULL, 0, cy / 2, cx, cy - cy / 2, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }

    case WM_SETFOCUS:
        if (logView_) {
            SetFocus(logView_);
        }
        return 0;

    case WM_NOTIFY:
        return OnNotify(reinterpret_cast<NMHDR*>(lParam));

    case WM_COMMAND:
        switch (GET_WM_COMMAND_ID(wParam, lParam)) {
        case ID_FILE_SENDFILE:
            break;
        }
        return 0;
    }

    return Window::HandleMessage(uMsg, wParam, lParam);
}

void RootWindow::SelectAndSendFile(const ContactData& contactData)
{
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
        diskThread_->Enqueue(contactData.c, filename);
    }
}

RootWindow *RootWindow::Create()
{
    RootWindow *self = new RootWindow();
    if (self->WinCreateWindow(0,
            TEXT("HomeShare"), WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            NULL, LoadMenu(g_hinst, MAKEINTRESOURCE(IDR_MENU1)))) {
        return self;
    }
    delete self;
    return nullptr;
}

void RootWindow::PaintContent(PAINTSTRUCT* pps) {
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

    int argc;
    LPWSTR* args = CommandLineToArgvW(GetCommandLine(), &argc);
    if (args[1]) {
        g_remoteIp = Utf16ToUtf8(args[1]);
    }

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
