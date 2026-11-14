#pragma once
#include <QtGlobal>
#include <qt_windows.h>

inline bool eraseBlackBackground(void *message, qintptr *result) {
    const auto *msg = static_cast<MSG *>(message);
    if (msg->message != WM_ERASEBKGND) return false;
    // Native windows can be exposed before Qt presents their first frame.
    RECT rect;
    GetClientRect(msg->hwnd, &rect);
    FillRect(reinterpret_cast<HDC>(msg->wParam), &rect,
        static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    *result = 1;
    return true;
}
