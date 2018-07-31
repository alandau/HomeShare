#pragma once

#include <windows.h>
#include <functional>
#include <thread>
#include <optional>

class MessageThread {
public:
    enum {WM_USER_MESSAGE = WM_USER + 10};
    MessageThread();
    virtual ~MessageThread();
    void RunInThread(std::function<void(void)> func);
    HWND GetHWND() const;

protected:
    virtual std::optional<LRESULT> HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
        return std::nullopt;
    }
    virtual void InitInThread() {}
private:
    void Loop();

    class MessageWindow;
    std::thread thread_;
    MessageWindow* win_;
};
