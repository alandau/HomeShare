#undef WINVER
#undef _WIN32_WINNT
#define WINVER 0x0600
#define _WIN32_WINNT 0x0600

#include "vista.h"
#include "raii.h"
#include <shobjidl.h>

bool VistaSelectFolder(HWND hwnd, std::wstring& path)
{
    IFileOpenDialog *pfd = NULL;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog,
        NULL,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&pfd));

    if (FAILED(hr)) {
        return false;
    }
    SCOPE_EXIT{
        pfd->Release();
    };

    DWORD flags;
    if (FAILED(pfd->GetOptions(&flags))) {
        return false;
    }
    if (FAILED(pfd->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_PICKFOLDERS))) {
        return false;
    }

    hr = pfd->Show(hwnd);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        // User cancelled
        path.clear();
        return true;
    }
    if (FAILED(hr)) {
        return false;
    }

    IShellItem* si;
    if (FAILED(pfd->GetResult(&si))) {
        return false;
    }
    SCOPE_EXIT{
        si->Release();
    };

    wchar_t* result;
    if (FAILED(si->GetDisplayName(SIGDN_FILESYSPATH, &result))) {
        return false;
    }

    path = result;
    CoTaskMemFree(result);
    return true;
}