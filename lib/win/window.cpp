#include "window.h"

HINSTANCE g_hinst;

void Window::Register()
{
    WNDCLASS wc;
    wc.style = 0;
    wc.lpfnWndProc = Window::s_WndProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = g_hinst;
    wc.hIcon = NULL;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszMenuName = NULL;
    wc.lpszClassName = ClassName();

    WinRegisterClass(&wc);
}

LRESULT CALLBACK Window::s_WndProc(
    HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    Window *self;
    if (uMsg == WM_NCCREATE) {
        LPCREATESTRUCT lpcs = reinterpret_cast<LPCREATESTRUCT>(lParam);
        self = reinterpret_cast<Window *>(lpcs->lpCreateParams);
        self->m_hwnd = hwnd;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LPARAM>(self));
    } else {
        self = reinterpret_cast<Window *>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->HandleMessage(uMsg, wParam, lParam);
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

LRESULT Window::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    LRESULT lres;

    switch (uMsg) {
    case WM_NCDESTROY:
        lres = DefWindowProc(m_hwnd, uMsg, wParam, lParam);
        SetWindowLongPtr(m_hwnd, GWLP_USERDATA, 0);
        delete this;
        return lres;

    case WM_PAINT:
        OnPaint();
        return 0;

    case WM_PRINTCLIENT:
        OnPrintClient(reinterpret_cast<HDC>(wParam));
        return 0;
    }

    return DefWindowProc(m_hwnd, uMsg, wParam, lParam);
}

void Window::OnPaint()
{
    PAINTSTRUCT ps;
    BeginPaint(m_hwnd, &ps);
    PaintContent(&ps);
    EndPaint(m_hwnd, &ps);
}

void Window::OnPrintClient(HDC hdc)
{
    PAINTSTRUCT ps;
    ps.hdc = hdc;
    GetClientRect(m_hwnd, &ps.rcPaint);
    PaintContent(&ps);
}
