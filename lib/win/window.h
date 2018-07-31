#pragma once

#include <windows.h>

extern HINSTANCE g_hinst;

class Window
{
public:
    HWND GetHWND() { return m_hwnd; }
protected:
    virtual LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);
    virtual void PaintContent(PAINTSTRUCT *pps) {}
    virtual LPCTSTR ClassName() = 0;
    virtual BOOL WinRegisterClass(WNDCLASS *pwc) {
        return RegisterClass(pwc);
    }
    virtual ~Window() { }

    HWND WinCreateWindow(DWORD dwExStyle, LPCTSTR pszName,
        DWORD dwStyle, int x, int y, int cx, int cy,
        HWND hwndParent, HMENU hmenu) {
        Register();
        return CreateWindowEx(dwExStyle, ClassName(), pszName, dwStyle,
            x, y, cx, cy, hwndParent, hmenu, g_hinst, this);
    }
private:
    void Register();
    void OnPaint();
    void OnPrintClient(HDC hdc);
    static LRESULT CALLBACK s_WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
protected:
    HWND m_hwnd;
};
