#include "AppBarWindow.h"

#include <cassert>
#include <iostream>

class AppBarWindowInteractionTests {
public:
    static void Run() {
        AppBarWindow widget(GetModuleHandleW(nullptr));
        widget.RegisterWindowClass();
        // A hidden HWND can never own the pointer, even when its rectangle
        // happens to contain the cursor. It models an occluded widget.
        widget.hwnd_ = CreateWindowExW(0, L"CodexUsageMonitorWindow", L"Interaction regression",
            WS_POPUP, 0, 0, 100, 100, nullptr, nullptr,
            GetModuleHandleW(nullptr), &widget);
        assert(widget.hwnd_ != nullptr);
        widget.presentationState_ = codex_widget::PresentationState::PinnedExpanded;
        widget.ArmMouseLeaveTracking();
        assert(!widget.mouseTracking_);

        POINT cursor = {};
        assert(GetCursorPos(&cursor));
        widget.savedRect_ = { cursor.x - 10, cursor.y - 10, cursor.x + 100, cursor.y + 100 };
        widget.mouseTracking_ = true;
        widget.HandleMessage(WM_MOUSELEAVE, 0, 0);
        assert(!widget.mouseTracking_);
        for (int i = 0; i < 100; ++i) widget.UpdateHoverStateFromCursor();
        assert(!widget.mouseTracking_);
        MSG message = {};
        assert(!PeekMessageW(&message, widget.hwnd_, WM_MOUSELEAVE, WM_MOUSELEAVE, PM_REMOVE));

        // Clicking an already selected tab must consume the input.
        widget.quotaTabRect_ = { 10, 10, 80, 40 };
        widget.activityTabRect_ = { 90, 10, 160, 40 };
        widget.fullPage_ = codex_widget::FullPage::Quota;
        assert(widget.TryHandleControlClick({ 20, 20 }));
        widget.fullPage_ = codex_widget::FullPage::Activity;
        assert(widget.TryHandleControlClick({ 100, 20 }));
        assert(!widget.TryHandleControlClick({ 85, 20 }));

        for (UINT cancellation : { WM_CAPTURECHANGED, WM_CANCELMODE }) {
            widget.dragMode_ = AppBarWindow::DragMode::Move;
            widget.bubbleClickPending_ = true;
            widget.dragMoved_ = false;
            widget.settingsDragging_ = true;
            widget.HandleMessage(cancellation, 0, 0);
            assert(widget.dragMode_ == AppBarWindow::DragMode::None);
            assert(!widget.bubbleClickPending_ && !widget.dragMoved_ && !widget.settingsDragging_);
            assert(widget.presentationState_ == codex_widget::PresentationState::PinnedExpanded);
        }

        widget.contextMenuOpen_ = true;
        widget.presentationState_ = codex_widget::PresentationState::HoverExpanded;
        widget.savedRect_ = { cursor.x + 1000, cursor.y + 1000, cursor.x + 1100, cursor.y + 1100 };
        widget.UpdateHoverStateFromCursor();
        assert(widget.presentationState_ == codex_widget::PresentationState::HoverExpanded);

        // Detach the test object before destroying the hidden test HWND so
        // production shutdown/settings persistence does not run in this test.
        SetWindowLongPtrW(widget.hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(widget.hwnd_);
        widget.hwnd_ = nullptr;
    }
};

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    AppBarWindowInteractionTests::Run();
    std::cout << "WindowInteractionTests passed\n";
}
