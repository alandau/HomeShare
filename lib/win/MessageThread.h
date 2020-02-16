#pragma once

#include <windows.h>
#include <thread>
#include <optional>

class MessageThread {
public:
    MessageThread();
    virtual ~MessageThread();
    void Start();
    void RunInThread(std::function<void(void)> func);
    LRESULT RunInThreadWithResult(std::function<LRESULT(void)> func);
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
    HANDLE readyEvent_;
};
