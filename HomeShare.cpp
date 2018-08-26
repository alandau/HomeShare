#include "lib/win/window.h"
#include "lib/win/raii.h"
#include "lib/win/encoding.h"
#include "SocketThread.h"
#include "DiskThread.h"
#include "DiscoveryThread.h"
#include "Logger.h"
#include "Database.h"
#include "lib/sodium.h"
#include "lib/crypto.h"
#include "resource.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>

static std::wstring GetDesktopPath() {
    wchar_t path[MAX_PATH];
    if (FAILED(SHGetFolderPath(NULL, CSIDL_DESKTOPDIRECTORY, NULL, SHGFP_TYPE_CURRENT, path))) {
        return L"";
    }
    return path;
}

static std::wstring GetAppDataPath() {
    wchar_t path[MAX_PATH];
    if (FAILED(SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, path))) {
        return L"";
    }
    return path;
}

static std::wstring formatSize(uint64_t done, uint64_t total) {
    enum { KB = 1024, MB = 1024 * 1024, GB = 1024 * 1024 * 1024 };
    if (total >= GB) {
        return fmt::format(L"{:.2f} / {:.2f} GB", (double)done / GB, (double)total / GB);
    } else if (total >= MB) {
        return fmt::format(L"{:.2f} / {:.2f} MB", (double)done / MB, (double)total / MB);
    } else if (total >= KB) {
        return fmt::format(L"{:.2f} / {:.2f} KB", (double)done / KB, (double)total / KB);
    } else {
        return fmt::format(L"{} / {} B", done, total);
    }
}

static std::wstring formatSpeed(double speed) {
    enum { KB = 1024, MB = 1024 * 1024, GB = 1024 * 1024 * 1024 };
    if (speed >= GB) {
        return fmt::format(L"{:.2f} GB/s", speed / GB);
    } else if (speed >= MB) {
        return fmt::format(L"{:.2f} MB/s", speed / MB);
    } else if (speed >= KB) {
        return fmt::format(L"{:.2f} KB/s", speed / KB);
    } else {
        return fmt::format(L"{:.2f} B/s", speed);
    }
}

class ListViewLogger : public Logger {
public:
    ListViewLogger(Window* window, HWND listView)
        : window_(window)
        , listView_(listView)
    {}
protected:
    void logString(LogLevel level, const std::wstring& s) override {
        if (level == F) {
            MessageBox(window_->GetHWND(), s.c_str(), L"Fatal Error", MB_ICONERROR | MB_OK);
            ExitProcess(1);
        }
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
        item.iImage = level == D ? -1 : (int)level - (int)E;
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
    // Static data coming from the database
    struct {
        Contact c;
        bool known = false;
        std::wstring displayName;
        std::string host;
    } stat;
    // Dynamic data updated on the fly
    struct {
        std::string host;
        uint16_t port;
        ConnectState connectState = ConnectState::Disconnected;
        ProgressUpdate prevProgress, progress;
    } dyn;
};

class RootWindow : public Window
{
public:
    LPCTSTR ClassName() override { return TEXT("HomeShare"); }
    static RootWindow *Create(const std::wstring& path);
    void SetDbPath(const std::wstring& path);
protected:
    LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override;
    void PaintContent(PAINTSTRUCT* pps) override;
    LRESULT OnCreate();
    LRESULT OnNotify(NMHDR* pnm);
    void OnGetDispInfo(NMLVDISPINFO* pnmv);
    LRESULT OnLVCustomDraw(NMLVCUSTOMDRAW* pcd);
private:
    HWND logView_;
    HWND contactView_;
    std::wstring dbPath_;
    std::vector<ContactData> contactData_;
    std::unique_ptr<ListViewLogger> logger_;
    std::unique_ptr<Database> db_;
    std::unique_ptr<SocketThreadApi> socketThread_;
    std::unique_ptr<DiskThread> diskThread_;
    std::unique_ptr<DiscoveryThread> discoveryThread_;

    void SelectAndSendFile(const ContactData& contactData);
    int GetContactIndex(const Contact& c);
    bool GetContactHostAndPort(const ContactData& c, std::string* hostname = nullptr, uint16_t* port = nullptr);
    void LoadContactsFromDb();
    void AddToContacts(const ContactData& c);
};

int RootWindow::GetContactIndex(const Contact& c) {
    for (int i = 0; i < (int)contactData_.size(); i++) {
        if (contactData_[i].stat.c == c) {
            return i;
        }
    }
    return -1;
}

bool RootWindow::GetContactHostAndPort(const ContactData& c, std::string* hostname, uint16_t* port) {
    if (!c.dyn.host.empty()) {
        if (hostname) {
            *hostname = c.dyn.host;
            *port = c.dyn.port;
        }
        return true;
    }
    if (!c.stat.host.empty()) {
        if (hostname) {
            *hostname = c.stat.host;
            *port = 8890;
        }
        return true;
    }
    return false;
}

void RootWindow::LoadContactsFromDb() {
    std::vector<Database::Contact> contacts = db_->GetContacts();
    for (const Database::Contact& c : contacts) {
        int index = GetContactIndex(Contact{ c.pubkey });
        if (index != -1) {
            ContactData& data = contactData_[index];
            data.stat.displayName = c.name;
            data.stat.host = c.host;
            data.stat.known = true;
        } else {
            ContactData data;
            data.stat.c = Contact{ c.pubkey };
            data.stat.displayName = c.name;
            data.stat.host = c.host;
            data.stat.known = true;
            contactData_.push_back(std::move(data));
        }
    }
    ListView_SetItemCount(contactView_, contactData_.size());
    ListView_RedrawItems(contactView_, 0, contactData_.size() - 1);
    UpdateWindow(contactView_);
}

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

    lvc.mask = LVCF_TEXT | LVCF_WIDTH;
    lvc.cx = 100;
    lvc.pszText = TEXT("Sent Files");
    ListView_InsertColumn(contactView_, 2, &lvc);

    lvc.mask = LVCF_TEXT | LVCF_WIDTH;
    lvc.cx = 150;
    lvc.pszText = TEXT("Sent Bytes");
    ListView_InsertColumn(contactView_, 3, &lvc);

    lvc.mask = LVCF_TEXT | LVCF_WIDTH;
    lvc.cx = 100;
    lvc.pszText = TEXT("Speed");
    ListView_InsertColumn(contactView_, 4, &lvc);

    logger_.reset(new ListViewLogger(this, logView_));

    db_.reset(new Database(*logger_));
    if (!db_->OpenOrCreate(dbPath_)) {
        MessageBox(GetHWND(), fmt::format(L"Can't open/create database at {}", dbPath_).c_str(), L"HomeShare", MB_ICONERROR | MB_OK);
        return -1;
    }

    std::string pub, priv;
    db_->GetKeys(&pub, &priv);
    logger_->i(L"My public key: {}", keyToDisplayStr(pub));

    LoadContactsFromDb();

    socketThread_.reset(new SocketThreadApi);
    socketThread_->Init(logger_.get(), pub, priv);
    socketThread_->setOnConnectCb([this](const Contact& c, bool connected) {
        RunInThread([this, c, connected] {
            int index = GetContactIndex(c);
            if (index == -1) {
                // Can happen if the remote end which is not a contact connected/disconnected to us
                return;
            }
            contactData_[index].dyn.connectState = connected
                ? ContactData::ConnectState::Connected
                : ContactData::ConnectState::Disconnected;
            ListView_RedrawItems(contactView_, index, index);
            UpdateWindow(contactView_);
        });
    });
    socketThread_->setIsKnownContact([this](const std::string& pubkey) {
        return !!RunInThreadWithResult([this, pubkey] {
            Contact c{pubkey};
            int index = GetContactIndex(c);
            bool found = index != -1 && contactData_[index].stat.known;
            if (!found) {
                RunInThread([this, pubkey] {
                    if (MessageBox(GetHWND(),
                        fmt::format(L"An unknown contact is trying to connect.\n\n"
                            "Add this contact to contact list?\n\nContact public key: {}", keyToDisplayStr(pubkey)).c_str(),
                        L"Connection from unknown contact", MB_OKCANCEL | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDOK) {
                        return;
                    }
                    ContactData d;
                    d.stat.c = Contact{pubkey};
                    d.stat.displayName = keyToDisplayStr(pubkey);
                    AddToContacts(d);
                });
            }
            return found;
        });
    });

    diskThread_.reset(new DiskThread(logger_.get(), socketThread_.get(), GetDesktopPath()));
    diskThread_->setProgressUpdateCb([this](const Contact& c, const ProgressUpdate& up) {
        RunInThread([this, c, up] {
            int index = GetContactIndex(c);
            if (index == -1) {
                return;
            }
            auto now = std::chrono::steady_clock::now();
            ContactData& data = contactData_[index];
            data.dyn.prevProgress = data.dyn.progress;
            data.dyn.progress = up;
            ListView_RedrawItems(contactView_, index, index);
            UpdateWindow(contactView_);
        });
    });
    diskThread_->Start();

    discoveryThread_.reset(new DiscoveryThread(*logger_, pub));
    discoveryThread_->setOnResult([this](const std::vector<DiscoveryThread::DiscoveryResult>& result) {
        RunInThread([this, result] {
            contactData_.erase(std::remove_if(contactData_.begin(), contactData_.end(), [this](const ContactData& c) {
                return !c.stat.known;
            }), contactData_.end());
            for (ContactData& c : contactData_) {
                c.dyn.host.clear();
            }
            for (const DiscoveryThread::DiscoveryResult& r : result) {
                int index = GetContactIndex(Contact{ r.pubkey });
                if (index != -1) {
                    // Update existing contact's dyn data
                    ContactData& c = contactData_[index];
                    c.dyn.host = r.host;
                    c.dyn.port = r.port;
                } else {
                    ContactData c;
                    c.stat.c.pubkey = r.pubkey;
                    c.stat.displayName = Utf8ToUtf16(fmt::format("{}:{}", r.host, r.port));
                    c.dyn.host = r.host;
                    c.dyn.port = r.port;
                    contactData_.push_back(std::move(c));
                }
            }
            ListView_SetItemCount(contactView_, contactData_.size());
            ListView_RedrawItems(contactView_, 0, contactData_.size() - 1);
            UpdateWindow(contactView_);
        });
    });
    discoveryThread_->Start();
    return 0;
}

LRESULT RootWindow::OnNotify(NMHDR *pnm) {
    if (pnm->hwndFrom == contactView_) {
        switch (pnm->code) {
        case LVN_GETDISPINFO:
            OnGetDispInfo(CONTAINING_RECORD(pnm, NMLVDISPINFO, hdr));
            break;
        case NM_CUSTOMDRAW:
            return OnLVCustomDraw(CONTAINING_RECORD(
                CONTAINING_RECORD(pnm, NMCUSTOMDRAW, hdr),
                NMLVCUSTOMDRAW, nmcd));
            break;
        case NM_RCLICK: {
            NMITEMACTIVATE* nma = CONTAINING_RECORD(pnm, NMITEMACTIVATE, hdr);
            if (nma->iItem == -1) {
                break;
            }
            ContactData& data = contactData_[nma->iItem];
            bool conn = data.dyn.connectState == ContactData::ConnectState::Connected;
            HMENU hMenu = CreatePopupMenu();
            AppendMenu(hMenu, MF_STRING | (conn ? MF_GRAYED : 0), 1, L"Connect");
            AppendMenu(hMenu, MF_STRING | (!conn ? MF_GRAYED : 0), 2, L"Disconnect");
            AppendMenu(hMenu, MF_STRING | (!conn ? MF_GRAYED : 0), 3, L"Send File");
            if (!data.stat.known) {
                AppendMenu(hMenu, MF_STRING, 4, L"Add to contacts");
            }
            POINT p;
            GetCursorPos(&p);
            int item = TrackPopupMenu(hMenu, TPM_RETURNCMD, p.x, p.y, 0, GetHWND(), NULL);
            switch (item) {
            case 1: {
                std::string hostname;
                uint16_t port;
                if (!GetContactHostAndPort(data, &hostname, &port)) {
                    break;
                }
                data.dyn.connectState = ContactData::ConnectState::Connecting;
                socketThread_->Connect(data.stat.c, hostname, port);
                ListView_RedrawItems(contactView_, nma->iItem, nma->iItem);
                UpdateWindow(contactView_);
                break;
            }
            case 2:
                socketThread_->Disconnect(data.stat.c);
                break;
            case 3:
                SelectAndSendFile(data);
                break;
            case 4:
                AddToContacts(data);
                break;
            }
            break;
        }
        }
    } else if (pnm->hwndFrom == logView_) {
        switch (pnm->code) {
        case NM_DBLCLK: {
            enum { MAX_LEN = 1000 };
            NMITEMACTIVATE* nma = CONTAINING_RECORD(pnm, NMITEMACTIVATE, hdr);
            if (nma->iItem == -1) {
                break;
            }
            if (OpenClipboard(GetHWND()) == NULL) {
                break;
            }
            EmptyClipboard();
            HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, MAX_LEN * sizeof(wchar_t));
            if (hGlob == NULL) {
                CloseClipboard();
                break;
            }
            wchar_t* buf = (wchar_t *)GlobalLock(hGlob);
            ListView_GetItemText(logView_, nma->iItem, 2, buf, MAX_LEN);
            GlobalUnlock(hGlob);
            SetClipboardData(CF_UNICODETEXT, hGlob);
            CloseClipboard();
            break;
        }
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
            pnmv->item.pszText = const_cast<LPWSTR>(data.stat.displayName.c_str());
            break;
        case 1:
            pnmv->item.pszText =
                data.dyn.connectState == ContactData::ConnectState::Connected ? L"Connected" :
                data.dyn.connectState == ContactData::ConnectState::Disconnected ? L"Disconnected" :
                data.dyn.connectState == ContactData::ConnectState::Connecting ? L"Connecting" : L"";
            break;
        case 2: {
            std::wstring res = fmt::format(L"{} / {}", data.dyn.progress.doneFiles, data.dyn.progress.totalFiles);
            lstrcpyn(pnmv->item.pszText, res.c_str(), pnmv->item.cchTextMax);
            break;
        }
        case 3: {
            std::wstring res = formatSize(data.dyn.progress.doneBytes, data.dyn.progress.totalBytes);
            lstrcpyn(pnmv->item.pszText, res.c_str(), pnmv->item.cchTextMax);
            break;
        }
        case 4: {
            if (data.dyn.prevProgress.timestamp == std::chrono::steady_clock::time_point()) {
                // First progress, ignore
                pnmv->item.pszText[0] = L'\0';
            } else {
                using float_seconds = std::chrono::duration<double>;
                uint64_t diffBytes = data.dyn.progress.doneBytes - data.dyn.prevProgress.doneBytes;
                std::chrono::steady_clock::duration diffTime = data.dyn.progress.timestamp - data.dyn.prevProgress.timestamp;
                double speed = diffBytes / float_seconds(diffTime).count();
                std::wstring res = formatSpeed(speed);
                lstrcpyn(pnmv->item.pszText, res.c_str(), pnmv->item.cchTextMax);
            }
            break;
        }
        }
    }
    
    if (pnmv->item.mask & LVIF_IMAGE) {
        pnmv->item.iImage = -1;
    }

    if (pnmv->item.mask & LVIF_STATE) {
        pnmv->item.state = 0;
    }
}

LRESULT RootWindow::OnLVCustomDraw(NMLVCUSTOMDRAW* pcd)
{
    switch (pcd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        if (pcd->nmcd.dwItemSpec < contactData_.size()) {
            const ContactData& c = contactData_[pcd->nmcd.dwItemSpec];
            if (c.dyn.connectState == ContactData::ConnectState::Connected) {
                pcd->clrTextBk = RGB(0, 255, 0);
            } else if (!c.stat.known) {
                pcd->clrText = RGB(255, 0, 0);
            } else if (GetContactHostAndPort(c)) {
                pcd->clrText = RGB(0, 128, 0);
            }
        }
        break;
        //return CDRF_NOTIFYSUBITEMDRAW;
#if 0
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
        pcd->clrText = m_clrTextNormal;
        if (pcd->iSubItem == COL_SIMP &&
            pcd->nmcd.dwItemSpec < (DWORD)Length()) {
            const DictionaryEntry& de = Item(pcd->nmcd.dwItemSpec);
            if (de.m_pszSimp) {
                pcd->clrText = RGB(0x80, 0x00, 0x00);
            }
        }
        break;
#endif
    }
    return CDRF_DODEFAULT;
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
            discoveryThread_->StartDiscovery();
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
        diskThread_->Enqueue(contactData.stat.c, filename);
    }
}

void RootWindow::AddToContacts(const ContactData& c) {
    db_->AddContact(c.stat.c.pubkey, L"Name " + keyToDisplayStr(c.stat.c.pubkey));
    LoadContactsFromDb();
}

RootWindow *RootWindow::Create(const std::wstring& path)
{
    RootWindow *self = new RootWindow();
    self->SetDbPath(path);
    if (self->WinCreateWindow(0,
            TEXT("HomeShare"), WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            NULL, LoadMenu(g_hinst, MAKEINTRESOURCE(IDR_MENU1)))) {
        return self;
    }
    // self will be deleted if window creation failed
    return nullptr;
}

void RootWindow::SetDbPath(const std::wstring& path) {
    dbPath_ = path;
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

    if (sodium_init() < 0) {
        MessageBox(NULL, L"Couldn't initialize libsodium", L"HomeShare", MB_OK);
        return 1;
    }
    
    g_hinst = hinst;

    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLine(), &argc);
    std::wstring dbpath = argc >= 2
        ? argv[1]
        : GetAppDataPath() + L"\\HomeShare.db";

    ComInit comInit;
    InitCommonControls();

    RootWindow* w = RootWindow::Create(dbpath);
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
