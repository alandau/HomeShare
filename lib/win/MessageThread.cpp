#include "MessageThread.h"
#include "window.h"

class MessageThread::MessageWindow : public Window
{
public:
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

protected:
    LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
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
{}

MessageThread::~MessageThread() {
    if (!thread_.joinable()) {
        return;
    }
    RunInThread([] {
        PostQuitMessage(0);
    });
    thread_.join();
}

void MessageThread::Start() {
    thread_ = std::thread([this] { Loop(); });
}

void MessageThread::RunInThread(std::function<void(void)> func) {
    win_->RunInThread(std::move(func));
}

LRESULT MessageThread::RunInThreadWithResult(std::function<LRESULT(void)> func) {
    return win_->RunInThreadWithResult(std::move(func));
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
