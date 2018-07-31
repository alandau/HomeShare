#include "MessageThread.h"
#include "window.h"

class MessageThread::MessageWindow : public Window
{
public:
    enum {
        WM_RUN_IN_THREAD = WM_USER,
    };

    LPCTSTR ClassName() override { return TEXT("MessageWindow"); }
    static MessageWindow *Create(MessageThread* thread) {
        MessageWindow* self = new MessageWindow();
        self->thread_ = thread;
        if (self->WinCreateWindow(0,
            TEXT(""), WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            HWND_MESSAGE, NULL)) {
            return self;
        }
        delete self;
        return nullptr;
    }

    void RunInThread(std::function<void(void)> func) {
        std::function<void(void)>* p = new std::function<void(void)>(std::move(func));
        PostMessage(GetHWND(), WM_RUN_IN_THREAD, (WPARAM)p, 0);
    }
protected:
    LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
        if (uMsg == WM_RUN_IN_THREAD) {
            std::function<void(void)>* func = (std::function<void(void)>*)wParam;
            (*func)();
            delete func;
            return 0;
        }
        std::optional<LRESULT> res = thread_->HandleMessage(uMsg, wParam, lParam);
        if (res) {
            return *res;
        }
        return Window::HandleMessage(uMsg, wParam, lParam);
    }

private:
    MessageThread* thread_;
};


MessageThread::MessageThread()
    : thread_(std::thread([this] { Loop(); }))
{}

MessageThread::~MessageThread() {
    RunInThread([] {
        PostQuitMessage(0);
    });
    thread_.join();
}

void MessageThread::RunInThread(std::function<void(void)> func) {
    win_->RunInThread(std::move(func));
}

HWND MessageThread::GetHWND() const {
    return win_->GetHWND();
}

void MessageThread::Loop() {
    win_ = MessageWindow::Create(this);

    InitInThread();

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}
