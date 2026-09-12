#include "ActivityDetailsWindow.h"
#include <CommCtrl.h>
#include <algorithm>
#include <cmath>
#include <commdlg.h>
#include <ctime>
#include <fstream>
#include <initguid.h>
#include <iomanip>
#include <oleacc.h>
#include <sstream>
#include <windowsx.h>
#include <uxtheme.h>
#pragma comment(                                                                                             \
    linker,                                                                                                  \
    "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
constexpr UINT kReady = WM_APP + 81;
constexpr UINT kExportReady = WM_APP + 82;
enum Id {
    Period = 100,
    Source,
    Theme,
    From,
    Until,
    Previous,
    Next,
    Today,
    Export,
    Copy,
    Help,
    Chart,
    Average,
    Compare,
    Search,
    Scope,
    Sort,
    Back,
    Forward,
    PageSize,
    Details,
    Alias,
    Tags,
    SaveAlias,
    Scan,
    Rebuild,
    Cancel,
    Goal,
    GoalEnabled,
    SaveGoal,
    Compact,
    Reset,
    Table,
    Status,
    ExportFormat,
    ExportTable,
    CardOrder,
    NavTrends = 200,
    NavTokens,
    NavTasks,
    NavQuality,
    NavPreferences
};
std::wstring Value(HWND w) {
    int n = GetWindowTextLengthW(w);
    std::wstring s(n + 1, 0);
    GetWindowTextW(w, s.data(), n + 1);
    s.resize(n);
    return s;
}
int Selection(HWND w) { return static_cast<int>(SendMessageW(w, CB_GETCURSEL, 0, 0)); }
void Select(HWND w, int i) { SendMessageW(w, CB_SETCURSEL, i, 0); }
void Choices(HWND w, std::initializer_list<const wchar_t *> items) {
    SendMessageW(w, CB_RESETCONTENT, 0, 0);
    for (auto s : items)
        SendMessageW(w, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    Select(w, 0);
}
long long PickerTime(HWND w, bool nextDay) {
    SYSTEMTIME s{};
    DateTime_GetSystemtime(w, &s);
    std::tm t{};
    t.tm_year = s.wYear - 1900;
    t.tm_mon = s.wMonth - 1;
    t.tm_mday = s.wDay + (nextDay ? 1 : 0);
    t.tm_isdst = -1;
    return activity::FromLocalTime(t);
}
void SetPicker(HWND w, long long v) {
    time_t stamp = v;
    std::tm t{};
    t = activity::LocalTime(stamp);
    SYSTEMTIME s{};
    s.wYear = static_cast<WORD>(t.tm_year + 1900);
    s.wMonth = static_cast<WORD>(t.tm_mon + 1);
    s.wDay = static_cast<WORD>(t.tm_mday);
    DateTime_SetSystemtime(w, GDT_VALID, &s);
}
void Fill(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}
void Label(HDC dc, const std::wstring &s, RECT r, HFONT f, COLORREF c,
           UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    auto old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r, flags);
    SelectObject(dc, old);
}
void Card(HDC dc, RECT r, COLORREF c) {
    auto b = CreateSolidBrush(c);
    auto ob = SelectObject(dc, b);
    auto op = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, 18, 18);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
}
std::wstring Decimal(long double v) {
    std::wostringstream o;
    o << std::fixed << std::setprecision(1) << v;
    return o.str();
}
std::wstring Integer(std::uint64_t v) { return activity::Number(v, true, true); }
std::wstring Percent(std::uint64_t a, std::uint64_t b) { return activity::Ratio(a, b, true, true); }
void CopyText(HWND owner, const std::wstring &s) {
    if (!OpenClipboard(owner))
        return;
    auto mem = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    if (mem) {
        auto p = GlobalLock(mem);
        if (p) {
            memcpy(p, s.c_str(), (s.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            EmptyClipboard();
            if (!SetClipboardData(CF_UNICODETEXT, mem))
                GlobalFree(mem);
        } else
            GlobalFree(mem);
    }
    CloseClipboard();
}
} // namespace
ActivityDetailsWindow::ActivityDetailsWindow() : worker_([this](std::stop_token s) { Worker(s); }) {}
ActivityDetailsWindow::~ActivityDetailsWindow() {
    exporter_.request_stop();
    if (exporter_.joinable())
        exporter_.join();
    Close();
    worker_.request_stop();
    queryStop_.request_stop();
    wake_.notify_all();
    if (worker_.joinable())
        worker_.join();
    if (font_)
        DeleteObject(font_);
    if (numberFont_)
        DeleteObject(numberFont_);
    if (titleFont_)
        DeleteObject(titleFont_);
    if (background_)
        DeleteObject(background_);
    if (cardBrush_)
        DeleteObject(cardBrush_);
}
void ActivityDetailsWindow::Close() {
    if (hwnd_)
        DestroyWindow(hwnd_);
}
bool ActivityDetailsWindow::Translate(MSG &m) {
    if (!hwnd_ || !IsWindowVisible(hwnd_) || (m.hwnd != hwnd_ && !IsChild(hwnd_, m.hwnd))) return false;
    const bool handled = IsDialogMessageW(hwnd_, &m) != FALSE;
    if (handled && m.message == WM_KEYDOWN && m.wParam == VK_TAB) {
        HWND focus = GetFocus(); RECT focused{}, viewport{};
        if (focus && IsChild(hwnd_, focus) && GetWindowRect(focus, &focused)) {
            MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&focused), 2); GetClientRect(hwnd_, &viewport);
            if (focused.top < 0) scrollOffset_ += focused.top - 8;
            else if (focused.bottom > viewport.bottom) scrollOffset_ += focused.bottom - viewport.bottom + 8;
            Layout(); InvalidateRect(hwnd_, nullptr, TRUE);
        }
    }
    return handled;
}
void ActivityDetailsWindow::Open(HWND owner, bool zh, const codex_usage::LocalUsageSnapshot &s,
                                 const std::wstring &path, std::function<void(int)> refresh) {
    chinese_ = zh;
    snapshot_ = s;
    settingsPath_ = path + L".activity-v1.ini";
    refresh_ = std::move(refresh);
    if (hwnd_) {
        ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
        Request();
        return;
    }
    theme_ = std::clamp(
        static_cast<int>(GetPrivateProfileIntW(L"activity", L"theme", 0, settingsPath_.c_str())), 0, 3);
    query_.period = static_cast<activity::Period>(std::clamp(
        static_cast<int>(GetPrivateProfileIntW(L"activity", L"period", 0, settingsPath_.c_str())), 0, 6));
    compact_ = GetPrivateProfileIntW(L"activity", L"compact", 1, settingsPath_.c_str()) != 0;
    showGoal_ = GetPrivateProfileIntW(L"activity", L"goal_enabled", 0, settingsPath_.c_str()) != 0;
    wchar_t goal[32]{};
    GetPrivateProfileStringW(L"activity", L"daily_goal", L"0", goal, 32, settingsPath_.c_str());
    dailyGoal_ = _wcstoui64(goal, nullptr, 10);
    page_ = std::clamp(
        static_cast<int>(GetPrivateProfileIntW(L"activity", L"page", 0, settingsPath_.c_str())), 0, 4);
    chart_ = std::clamp(
        static_cast<int>(GetPrivateProfileIntW(L"activity", L"chart", 0, settingsPath_.c_str())), 0,
        page_ == 0   ? 6
        : page_ == 1 ? 4
                     : 1);
    cardOrder_ = std::clamp(
        static_cast<int>(GetPrivateProfileIntW(L"activity", L"card_order", 0, settingsPath_.c_str())), 0, 2);
    average_ = GetPrivateProfileIntW(L"activity", L"average", 1, settingsPath_.c_str()) != 0;
    compare_ = GetPrivateProfileIntW(L"activity", L"compare", 0, settingsPath_.c_str()) != 0;
    INITCOMMONCONTROLSEX common{sizeof(common),
                                ICC_LISTVIEW_CLASSES | ICC_DATE_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&common);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = Proc;
    wc.lpszClassName = L"CodexActivityAnalysis";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    int width = std::min(static_cast<LONG>(MulDiv(1180, GetDpiForWindow(owner), 96)),
                         mi.rcWork.right - mi.rcWork.left),
        height = std::min(static_cast<LONG>(MulDiv(840, GetDpiForWindow(owner), 96)),
                          mi.rcWork.bottom - mi.rcWork.top);
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_CONTROLPARENT, wc.lpszClassName,
                            Text(L"Activity analysis", L"活動分析"), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VSCROLL,
                            mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - width) / 2,
                            mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - height) / 2, width, height,
                            owner, nullptr, wc.hInstance, this);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    Request();
}
void ActivityDetailsWindow::Update(const codex_usage::LocalUsageSnapshot &s, bool zh) {
    snapshot_ = s;
    bool changed = chinese_ != zh;
    chinese_ = zh;
    if (hwnd_) {
        if (changed) {
            Close();
            return;
        }
        Request();
    }
}
LRESULT CALLBACK ActivityDetailsWindow::Proc(HWND w, UINT m, WPARAM a, LPARAM b) {
    auto self = reinterpret_cast<ActivityDetailsWindow *>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        self = static_cast<ActivityDetailsWindow *>(reinterpret_cast<CREATESTRUCTW *>(b)->lpCreateParams);
        self->hwnd_ = w;
        SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->Handle(m, a, b) : DefWindowProcW(w, m, a, b);
}
LRESULT ActivityDetailsWindow::Handle(UINT m, WPARAM a, LPARAM b) {
    switch (m) {
    case WM_CREATE:
        CreateControls();
        ApplyTheme();
        Layout();
        return 0;
    case WM_SIZE:
        Layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto p = reinterpret_cast<MINMAXINFO *>(b);
        p->ptMinTrackSize = {MulDiv(900, dpi_, 96), MulDiv(440, dpi_, 96)};
        return 0;
    }
    case WM_MOUSEWHEEL:
        scrollOffset_ -= GET_WHEEL_DELTA_WPARAM(a) * MulDiv(64, dpi_, 96) / WHEEL_DELTA;
        Layout(); InvalidateRect(hwnd_, nullptr, TRUE); return 0;
    case WM_VSCROLL: {
        SCROLLINFO scroll{sizeof(scroll), SIF_ALL}; GetScrollInfo(hwnd_, SB_VERT, &scroll);
        switch (LOWORD(a)) {
        case SB_LINEUP: scrollOffset_ -= MulDiv(32, dpi_, 96); break;
        case SB_LINEDOWN: scrollOffset_ += MulDiv(32, dpi_, 96); break;
        case SB_PAGEUP: scrollOffset_ -= scroll.nPage; break;
        case SB_PAGEDOWN: scrollOffset_ += scroll.nPage; break;
        case SB_THUMBTRACK: scrollOffset_ = scroll.nTrackPos; break;
        default: break;
        }
        Layout(); InvalidateRect(hwnd_, nullptr, TRUE); return 0;
    }
    case WM_DPICHANGED: {
        auto r = reinterpret_cast<RECT *>(b);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ApplyTheme();
        Layout();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT p;
        HDC dc = BeginPaint(hwnd_, &p);
        RECT r{};
        GetClientRect(hwnd_, &r);
        HDC buffer = CreateCompatibleDC(dc);
        auto bitmap = CreateCompatibleBitmap(dc, r.right, r.bottom);
        auto old = SelectObject(buffer, bitmap);
        Paint(buffer);
        BitBlt(dc, 0, 0, r.right, r.bottom, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, old);
        DeleteObject(bitmap);
        DeleteDC(buffer);
        EndPaint(hwnd_, &p);
        return 0;
    }
    case WM_COMMAND:
        Command(LOWORD(a), HIWORD(a));
        return 0;
    case WM_NOTIFY: {
        auto n = reinterpret_cast<NMHDR *>(b);
        if (n->code == DTN_DATETIMECHANGE && query_.period == activity::Period::Custom) {
            query_.start = PickerTime(Control(From), false);
            query_.end = PickerTime(Control(Until), true);
            Request();
        }
        if (n->idFrom == Table && n->code == NM_DBLCLK && page_ == 2)
            Command(Details, 0);
        if (n->idFrom == Table && n->code == LVN_KEYDOWN && page_ == 2 &&
            reinterpret_cast<NMLVKEYDOWN *>(b)->wVKey == VK_RETURN)
            Command(Details, 0);
        if (n->idFrom == Table && n->code == LVN_ITEMCHANGED && page_ == 2 && !populating_) {
            EditTask(false);
            const bool selected = ListView_GetNextItem(Control(Table), -1, LVNI_SELECTED) >= 0;
            EnableWindow(Control(Details), selected); EnableWindow(Control(SaveAlias), selected);
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(a);
        SetTextColor(dc, ink_);
        SetBkColor(dc, card_);
        return reinterpret_cast<LRESULT>(cardBrush_);
    }
    case WM_DRAWITEM: {
        auto d = reinterpret_cast<DRAWITEMSTRUCT *>(b);
        bool nav = d->CtlID >= NavTrends && d->CtlID <= NavPreferences;
        bool selected = nav && static_cast<int>(d->CtlID - NavTrends) == page_;
        Fill(d->hDC, d->rcItem, bg_);
        Card(d->hDC, d->rcItem, selected ? accent_ : card_);
        RECT t = d->rcItem;
        InflateRect(&t, -10, 0);
        Label(d->hDC, Value(d->hwndItem), t, font_, (d->itemState & ODS_DISABLED) ? muted_ : selected ? RGB(255, 255, 255) : ink_,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (d->itemState & ODS_FOCUS) {
            RECT f = d->rcItem;
            InflateRect(&f, -3, -3);
            DrawFocusRect(d->hDC, &f);
        }
        return TRUE;
    }
    case WM_LBUTTONUP: {
        POINT p{GET_X_LPARAM(b), GET_Y_LPARAM(b) + scrollOffset_};
        for (auto &[r, day] : chartHits_)
            if (PtInRect(&r, p)) {
                query_.period = activity::Period::Custom;
                query_.start = day;
                query_.end = activity::ShiftDays(day, 1);
                Select(Control(Period), 7);
                SetPicker(Control(From), day);
                SetPicker(Control(Until), day);
                Request();
                break;
            }
        return 0;
    }
    case kReady: {
        std::lock_guard lock(mutex_);
        if (completed_ && completedGeneration_ == generation_) {
            result_ = std::move(*completed_);
            completed_.reset();
            pending_ = false;
            Populate();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case kExportReady:
        exportBusy_ = false;
        EnableWindow(Control(Export), TRUE);
        SetWindowTextW(Control(Status),
                       a ? Text(L"Export complete", L"匯出完成")
                         : Text(L"Export failed; source data is unchanged", L"匯出失敗，來源資料保持原狀"));
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd_, SW_HIDE);
        return 0;
    case WM_DESTROY:
        SavePreferences();
        hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd_, m, a, b);
}
void ActivityDetailsWindow::CreateControls() {
    auto make = [&](int id, const wchar_t *type, const wchar_t *name, DWORD style) {
        return CreateWindowExW(0, type, name, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 20, 20, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr),
                               nullptr);
    };
    auto button = [&](int id, const wchar_t *en, const wchar_t *zh) {
        make(id, L"BUTTON", Text(en, zh), BS_OWNERDRAW);
    };
    button(NavTrends, L"Trends", L"趨勢");
    button(NavTokens, L"Token", L"Token 分析");
    button(NavTasks, L"Tasks", L"任務");
    button(NavQuality, L"Data quality", L"資料品質");
    button(NavPreferences, L"Preferences", L"偏好");
    for (int id : {Period, Source, Theme, Chart, Scope, Sort, PageSize, ExportFormat, ExportTable, CardOrder})
        make(id, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL);
    Choices(Control(Period),
            {Text(L"Today", L"今日"), Text(L"Yesterday", L"昨日"), Text(L"Last 7 days", L"近 7 日"),
             Text(L"Last 30 days", L"近 30 日"), Text(L"This week", L"本週"), Text(L"This month", L"本月"),
             Text(L"All records", L"全部紀錄"), Text(L"Custom dates", L"自訂日期")});
    Select(Control(Period), static_cast<int>(query_.period));
    Choices(Control(Source), {Text(L"All sources", L"全部來源"), Text(L"Explicit top-level", L"已確認頂層"),
                              Text(L"Non-top-level", L"非頂層"), Text(L"Unknown source", L"未辨識來源")});
    Choices(Control(Theme), {Text(L"Porcelain", L"瓷白"), Text(L"Midnight", L"午夜"),
                             Text(L"Forest", L"森林"), Text(L"Plum", L"紫暮")});
    Select(Control(Theme), theme_);
    make(From, DATETIMEPICK_CLASSW, Text(L"Start date", L"開始日期"), DTS_SHORTDATEFORMAT);
    make(Until, DATETIMEPICK_CLASSW, Text(L"End date", L"結束日期"), DTS_SHORTDATEFORMAT);
    DateTime_SetFormat(Control(From), L"yyyy/MM/dd");
    DateTime_SetFormat(Control(Until), L"yyyy/MM/dd");
    SetPicker(Control(From), std::time(nullptr));
    SetPicker(Control(Until), std::time(nullptr));
    button(Previous, L"Previous", L"上一期");
    button(Next, L"Next", L"下一期");
    button(Today, L"Today", L"回到今日");
    button(Export, L"Export", L"匯出");
    button(Copy, L"Copy values", L"複製數值");
    button(Help, L"Definitions", L"計算說明");
    make(Average, L"BUTTON", Text(L"7-day average", L"7 日均線"), BS_AUTOCHECKBOX);
    SendMessageW(Control(Average), BM_SETCHECK, average_ ? BST_CHECKED : BST_UNCHECKED, 0);
    make(Compare, L"BUTTON", Text(L"Previous period", L"前一期"), BS_AUTOCHECKBOX);
    SendMessageW(Control(Compare), BM_SETCHECK, compare_ ? BST_CHECKED : BST_UNCHECKED, 0);
    Choices(Control(CardOrder),
            {Text(L"Tokens first", L"Token 卡片優先"), Text(L"Tasks first", L"任務卡片優先"),
             Text(L"Turns first", L"回合卡片優先")});
    Select(Control(CardOrder), cardOrder_);
    make(Search, L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER);
    SendMessageW(Control(Search), EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(Text(L"Search ID, alias or tag", L"搜尋代號、別名或標籤")));
    Choices(Control(Scope), {Text(L"All sessions", L"所有工作階段"), Text(L"Top-level tasks", L"頂層任務"),
                             Text(L"Token only", L"僅有 Token 紀錄")});
    Choices(Control(Sort), {Text(L"Most tokens", L"Token 由多到少"), Text(L"Most recent", L"最近有紀錄"),
                            Text(L"Most turns", L"回合由多到少")});
    Choices(Control(PageSize), {L"25", L"50", L"100"});
    Select(Control(PageSize), 1);
    button(Back, L"‹ Previous", L"‹ 上一頁");
    button(Forward, L"Next ›", L"下一頁 ›");
    button(Details, L"Analyze task", L"分析此任務");
    button(SaveAlias, L"Save label", L"儲存別名");
    for (int id : {Alias, Tags, Goal})
        make(id, L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER);
    SendMessageW(Control(Alias), EM_SETLIMITTEXT, 60, 0);
    SendMessageW(Control(Tags), EM_SETLIMITTEXT, 100, 0);
    SendMessageW(Control(Alias), EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(Text(L"Local alias", L"本機別名")));
    SendMessageW(Control(Tags), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(Text(L"Tags", L"標籤")));
    button(Scan, L"Scan local", L"重新掃描本機");
    button(Rebuild, L"Rebuild index", L"重建本機索引");
    button(Cancel, L"Cancel scan", L"取消掃描");
    button(SaveGoal, L"Save preferences", L"儲存偏好");
    button(Reset, L"Reset filters", L"重設篩選");
    make(GoalEnabled, L"BUTTON", Text(L"Daily reference (local only)", L"啟用每日本機參考上限"),
         BS_AUTOCHECKBOX);
    SendMessageW(Control(GoalEnabled), BM_SETCHECK, showGoal_ ? BST_CHECKED : BST_UNCHECKED, 0);
    make(Compact, L"BUTTON", Text(L"Compact numbers", L"顯示精簡數字"), BS_AUTOCHECKBOX);
    SendMessageW(Control(Compact), BM_SETCHECK, compact_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(Control(Goal), std::to_wstring(dailyGoal_).c_str());
    Choices(Control(ExportFormat), {L"CSV", L"JSON"});
    Choices(Control(ExportTable), {Text(L"Summary", L"期間摘要"), Text(L"Daily rows", L"每日分布"),
                                   Text(L"Task rows", L"任務明細"), Text(L"Data quality", L"資料品質")});
    auto table = make(Table, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS);
    ListView_SetExtendedListViewStyle(table, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    make(Status, L"STATIC", L"", SS_LEFT);
    Populate();
    IAccPropServices *accessibility = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&accessibility)))) {
        const std::pair<int, const wchar_t *> names[] = {
            {Period, Text(L"Period", L"統計期間")},
            {Source, Text(L"Source filter", L"來源篩選")},
            {Theme, Text(L"Theme", L"介面主題")},
            {Chart, Text(L"Analysis view", L"分析項目")},
            {Search, Text(L"Search tasks", L"搜尋任務")},
            {Scope, Text(L"Task scope", L"任務範圍")},
            {Sort, Text(L"Sort tasks", L"任務排序")},
            {PageSize, Text(L"Rows per page", L"每頁列數")},
            {Alias, Text(L"Local alias", L"本機別名")},
            {Tags, Text(L"Local tags", L"本機標籤")},
            {Goal, Text(L"Daily Token reference", L"每日 Token 參考上限")},
            {ExportTable, Text(L"Export table", L"匯出內容")},
            {ExportFormat, Text(L"Export format", L"匯出格式")},
            {From, Text(L"Start date", L"開始日期")},
            {Until, Text(L"End date", L"結束日期")},
            {Table, Text(L"Activity data table", L"活動資料表")}};
        for (auto [id, name] : names)
            accessibility->SetHwndPropStr(Control(id), OBJID_CLIENT, CHILDID_SELF, PROPID_ACC_NAME, name);
        accessibility->Release();
    }
}
void ActivityDetailsWindow::ApplyTheme() {
    const COLORREF palettes[][6] = {{RGB(245, 246, 250), RGB(255, 255, 255), RGB(27, 35, 54),
                                     RGB(96, 107, 125), RGB(64, 98, 214), RGB(225, 230, 239)},
                                    {RGB(17, 23, 36), RGB(27, 36, 53), RGB(236, 241, 250), RGB(164, 178, 199),
                                     RGB(81, 132, 222), RGB(44, 57, 78)},
                                    {RGB(17, 35, 32), RGB(26, 49, 43), RGB(230, 244, 235), RGB(164, 190, 177),
                                     RGB(45, 137, 105), RGB(48, 76, 64)},
                                    {RGB(36, 25, 43), RGB(52, 36, 61), RGB(246, 232, 248), RGB(199, 172, 204),
                                     RGB(143, 87, 169), RGB(79, 54, 89)}};
    auto p = palettes[theme_];
    bg_ = p[0];
    card_ = p[1];
    ink_ = p[2];
    muted_ = p[3];
    accent_ = p[4];
    line_ = p[5];
    if (background_)
        DeleteObject(background_);
    if (cardBrush_)
        DeleteObject(cardBrush_);
    background_ = CreateSolidBrush(bg_);
    cardBrush_ = CreateSolidBrush(card_);
    dpi_ = GetDpiForWindow(hwnd_);
    if (font_)
        DeleteObject(font_);
    if (numberFont_)
        DeleteObject(numberFont_);
    if (titleFont_)
        DeleteObject(titleFont_);
    font_ = CreateFontW(-MulDiv(15, dpi_, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                        CLEARTYPE_QUALITY, 0, chinese_ ? L"Iansui" : L"Quantico");
    numberFont_ = CreateFontW(-MulDiv(29, dpi_, 96), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              0, 0, CLEARTYPE_QUALITY, 0, L"Quantico");
    titleFont_ = CreateFontW(-MulDiv(26, dpi_, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             0, 0, CLEARTYPE_QUALITY, 0, chinese_ ? L"Iansui" : L"Quantico");
    for (auto c = GetWindow(hwnd_, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    ListView_SetBkColor(Control(Table), card_);
    ListView_SetTextBkColor(Control(Table), card_);
    ListView_SetTextColor(Control(Table), ink_);
    for (int id : {Average, Compare, Compact, GoalEnabled}) SetWindowTheme(Control(id), L"", L"");
    InvalidateRect(hwnd_, nullptr, TRUE);
}
void ActivityDetailsWindow::Layout() {
    if (!Control(Table))
        return;
    RECT r{};
    GetClientRect(hwnd_, &r);
    const int viewportHeight = r.bottom;
    r.bottom = std::max<LONG>(r.bottom, MulDiv(710, dpi_, 96));
    scrollOffset_ = std::clamp<int>(scrollOffset_, 0, r.bottom - viewportHeight);
    SCROLLINFO scroll{sizeof(scroll), SIF_RANGE | SIF_PAGE | SIF_POS};
    scroll.nMax = r.bottom - 1; scroll.nPage = viewportHeight; scroll.nPos = scrollOffset_;
    SetScrollInfo(hwnd_, SB_VERT, &scroll, TRUE);
    auto scale = [&](int v) { return MulDiv(v, dpi_, 96); };
    int margin = scale(24), side = scale(154), left = side + margin, width = r.right - left - margin;
    int h = scale(34);
    const bool narrowTasks = width < scale(850);
    auto place = [&](int id, int x, int y, int w, int height, bool visible = true) {
        ShowWindow(Control(id), visible ? SW_SHOW : SW_HIDE);
        if (visible)
            MoveWindow(Control(id), x, y - scrollOffset_, w, height, TRUE);
    };
    for (int i = 0; i < 5; ++i)
        place(NavTrends + i, scale(16), scale(124 + i * 50), side - scale(20), scale(38));
    place(Theme, r.right - margin - scale(140), scale(26), scale(140), scale(200));
    place(Period, left, scale(92), scale(142), scale(270));
    place(Source, left + scale(152), scale(92), scale(156), scale(200));
    place(Previous, left + scale(318), scale(92), scale(76), h);
    place(Next, left + scale(402), scale(92), scale(76), h);
    place(Today, left + scale(486), scale(92), scale(100), h);
    place(Reset, left + width - scale(116), scale(92), scale(116), h);
    bool custom = query_.period == activity::Period::Custom;
    place(From, left, scale(134), scale(150), h, custom);
    place(Until, left + scale(162), scale(134), scale(150), h, custom);
    int top = scale(300);
    place(Chart, left, top, scale(194), scale(240), page_ == 0 || page_ == 1 || page_ == 3);
    place(Average, left + scale(208), top, scale(150), h, page_ == 0);
    place(Compare, left + scale(366), top, scale(150), h, page_ == 0);
    place(Search, left, top, scale(220), h, page_ == 2);
    place(Scope, left + scale(230), top, scale(160), scale(180), page_ == 2);
    place(Sort, left + scale(400), top, scale(190), scale(180), page_ == 2);
    place(Scan, left, scale(342), scale(146), h, page_ == 3);
    place(Rebuild, left + scale(156), scale(342), scale(146), h, page_ == 3);
    place(Cancel, left + scale(312), scale(342), scale(126), h, page_ == 3);
    place(GoalEnabled, left, scale(310), scale(280), h, page_ == 4);
    place(Goal, left, scale(354), scale(220), h, page_ == 4);
    place(Compact, left, scale(402), scale(220), h, page_ == 4);
    place(CardOrder, left + scale(240), scale(402), scale(200), scale(180), page_ == 4);
    place(SaveGoal, left + scale(240), scale(354), scale(152), h, page_ == 4);
    chartRect_ = {left, scale(page_ == 3 ? 394 : 344), left + width, scale(510)};
    int tableY = scale(page_ == 2   ? 344
                       : page_ == 3 ? 436
                                    : 530),
        bottom = r.bottom - scale(page_ == 2 ? (narrowTasks ? 204 : 154) : 104);
    place(Table, left, tableY, width, std::max(scale(65), bottom - tableY), page_ != 4);
    place(Status, left, r.bottom - scale(96), width, scale(24));
    const int taskNavigationY = r.bottom - scale(narrowTasks ? 194 : 144);
    place(Back, left, taskNavigationY, scale(110), h, page_ == 2);
    place(Forward, left + scale(120), taskNavigationY, scale(110), h, page_ == 2);
    place(PageSize, left + scale(240), taskNavigationY, scale(70), scale(160), page_ == 2);
    place(Details, left + scale(326), taskNavigationY, scale(140), h, page_ == 2);
    place(Alias, left + scale(narrowTasks ? 0 : 480), r.bottom - scale(144), scale(130), h, page_ == 2);
    place(Tags, left + scale(narrowTasks ? 140 : 620), r.bottom - scale(144), scale(108), h, page_ == 2);
    place(SaveAlias, left + scale(narrowTasks ? 258 : 738), r.bottom - scale(144), scale(110), h, page_ == 2);
    place(Help, scale(16), r.bottom - scale(64), side - scale(20), h);
    place(Copy, left, r.bottom - scale(58), scale(126), h);
    place(ExportTable, left + scale(146), r.bottom - scale(58), scale(160), scale(160));
    place(ExportFormat, left + scale(316), r.bottom - scale(58), scale(84), scale(130));
    place(Export, left + scale(410), r.bottom - scale(58), scale(96), h);
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void ActivityDetailsWindow::Paint(HDC dc) {
    RECT r{};
    GetClientRect(hwnd_, &r);
    r.bottom = std::max<LONG>(r.bottom, MulDiv(710, dpi_, 96));
    SetViewportOrgEx(dc, 0, -scrollOffset_, nullptr);
    Fill(dc, r, bg_);
    auto z = [&](int v) { return MulDiv(v, dpi_, 96); };
    int left = z(178), width = r.right - left - z(24);
    Label(dc, L"CODEX", {z(20), z(25), z(155), z(55)}, font_, accent_);
    Label(dc, Text(L"LOCAL ACTIVITY", L"本機活動"), {z(20), z(57), z(155), z(84)}, font_, muted_);
    Label(dc, Text(L"Activity analysis", L"活動分析"), {left, z(20), r.right - z(190), z(57)}, titleFont_,
          ink_);
    std::wstring subtitle =
        query_.session.empty()
            ? Text(L"A clear view of your recorded usage", L"清楚掌握本機紀錄的使用情況")
            : Text(L"Focused task · reset filters to return", L"單一任務分析 · 重設篩選可返回");
    Label(dc, subtitle, {left, z(57), r.right - z(180), z(81)}, font_, muted_);
    std::wstring dates = activity::Date(result_.start) + L" — " + activity::Date(result_.end - 1) + L"  ·  " +
                         activity::Timezone();
    if (query_.period != activity::Period::Custom)
        Label(dc, dates, {left, z(133), left + width, z(163)}, font_, muted_);
    const wchar_t *labels[] = {Text(L"Recorded tokens", L"期間 Token"), Text(L"Active tasks", L"活躍任務"),
                               Text(L"User turns", L"使用者回合")};
    std::uint64_t values[] = {result_.tokens.totalTokens, result_.activeTasks, result_.turns},
                  before[] = {result_.previous.totalTokens, result_.previousTasks, result_.previousTurns};
    for (int i = 0; i < 3; ++i) {
        const int metric = (i + cardOrder_) % 3;
        int x = left + i * (width + z(14)) / 3;
        RECT card{x, z(178), x + (width - z(28)) / 3, z(282)};
        Card(dc, card, card_);
        RECT text{card.left + z(18), card.top + z(10), card.right - z(12), card.top + z(33)};
        Label(dc, labels[metric], text, font_, muted_);
        text.top = card.top + z(34);
        text.bottom = card.top + z(72);
        Label(dc, result_.available ? activity::Number(values[metric], chinese_, !compact_) : L"—", text,
              chinese_ && compact_ ? titleFont_ : numberFont_, ink_);
        text.top = card.top + z(75);
        text.bottom = card.bottom - z(6);
        Label(dc, activity::Change(values[metric], before[metric], result_.comparisonKnown, chinese_), text,
              font_, muted_);
    }
    if (page_ == 0 || page_ == 1)
        DrawChart(dc, chartRect_);
    if (page_ == 2) {
    } else if (page_ == 3) {
        Label(dc,
              Text(L"Recorded data is local and may be incomplete. No conversation "
                   L"content is retained.",
                   L"統計來自本機紀錄，可能不完整；不保留對話內容。"),
              chartRect_, font_, muted_, DT_LEFT | DT_WORDBREAK);
    } else if (page_ == 4) {
        RECT t{left, z(468), left + width, z(620)};
        std::wstring msg = Text(L"Four coordinated themes. Your selections stay on this "
                                L"computer.\nDaily references are informational; they do not "
                                L"limit the model or represent account quota.",
                                L"四款一致設計的主題，偏好只儲存在本機。\n每日參考上限僅供對照，不"
                                L"限制模型，也不代表帳戶配額。");
        if (showGoal_ && dailyGoal_)
            msg += std::wstring(L"\n") + Text(L"Today: ", L"今日：") +
                   activity::Number(snapshot_.today.totalTokens, chinese_) + L" / " +
                   activity::Number(dailyGoal_, chinese_) + L" Token" +
                   (snapshot_.today.totalTokens > dailyGoal_ ? Text(L" · Above reference", L" · 已超過參考值")
                                                             : L"");
        Label(dc, msg, t, font_, muted_, DT_LEFT | DT_WORDBREAK);
    }
}
void ActivityDetailsWindow::DrawChart(HDC dc, RECT r) {
    chartHits_.clear();
    Card(dc, r, card_);
    auto pad = MulDiv(16, dpi_, 96);
    InflateRect(&r, -pad, -pad);
    if (!result_.available) {
        Label(dc, Text(L"Waiting for local records", L"等待本機紀錄"), r, font_, muted_);
        return;
    }
    if (page_ == 1) {
        if (chart_ == 3 && !result_.days.empty()) {
            Label(dc,
                  Text(L"Cached / input · blue     Reasoning / output · amber",
                       L"快取輸入／輸入 · 主題色　推理輸出／輸出 · 琥珀色"),
                  {r.left, r.top, r.right, r.top + 24}, font_, muted_);
            r.top += 32;
            for (int component = 0; component < 2; ++component) {
                auto pen = CreatePen(PS_SOLID, 2, component ? RGB(194, 136, 62) : accent_);
                auto old = SelectObject(dc, pen);
                bool started = false;
                for (size_t i = 0; i < result_.days.size(); ++i) {
                    const auto &b = result_.days[i];
                    const auto denominator = component ? b.tokens.outputTokens : b.tokens.inputTokens;
                    const auto numerator =
                        component ? b.tokens.reasoningOutputTokens : b.tokens.cachedInputTokens;
                    const unsigned mask = component ? 24 : 3;
                    if (!b.hasTokens || (b.validFields & mask) != mask || !denominator ||
                        numerator > denominator) {
                        started = false;
                        continue;
                    }
                    const int x = r.left + static_cast<int>(i * (r.right - r.left) /
                                                            std::max<size_t>(1, result_.days.size() - 1));
                    const int y = r.bottom - static_cast<int>(static_cast<long double>(numerator) /
                                                              denominator * (r.bottom - r.top));
                    if (started)
                        LineTo(dc, x, y);
                    else
                        MoveToEx(dc, x, y, nullptr);
                    started = true;
                    Ellipse(dc, x - 2, y - 2, x + 3, y + 3);
                }
                SelectObject(dc, old);
                DeleteObject(pen);
            }
            return;
        }
        if (chart_ == 0 && (result_.validFields & 9) == 9) {
            const auto input = result_.tokens.inputTokens, output = result_.tokens.outputTokens;
            const long double sum = static_cast<long double>(input) + output;
            if (sum) {
                const int split = r.left + static_cast<int>(input / sum * (r.right - r.left));
                Fill(dc, {r.left, r.top, split, r.top + 16}, accent_);
                Fill(dc, {split, r.top, r.right, r.top + 16}, RGB(194, 136, 62));
                r.top += 24;
            }
        }
        auto s = activity::Summary(result_, chinese_) + L"\n\n" +
                 Text(L"Cached input and reasoning are subsets, not extra usage.",
                      L"快取輸入與推理輸出屬於分項，不再加回總量。");
        Label(dc, s, r, font_, ink_, DT_WORDBREAK);
        return;
    }
    if (chart_ == 3) {
        int gap = 3, cell = std::min((r.right - r.left) / 54, (r.bottom - r.top) / 7);
        std::tm first{};
        if (!result_.calendar.empty()) {
            const time_t t = result_.calendar.front().start;
            first = activity::LocalTime(t);
        }
        int index = (first.tm_wday + 6) % 7;
        std::uint64_t peak = 1;
        for (const auto &b : result_.calendar)
            peak = std::max(peak, b.tokens.totalTokens);
        for (auto &b : result_.calendar) {
            int col = index / 7, row = index % 7;
            RECT c{r.left + col * cell, r.top + row * cell, r.left + (col + 1) * cell - gap,
                   r.top + (row + 1) * cell - gap};
            const double weight = b.tokens.totalTokens
                                      ? .2 + .8 * std::sqrt(static_cast<double>(b.tokens.totalTokens) / peak)
                                      : 0;
            auto mix = [&](BYTE a, BYTE z) { return static_cast<BYTE>(a + (z - a) * weight); };
            Fill(dc, c,
                 b.known ? RGB(mix(GetRValue(line_), GetRValue(accent_)),
                               mix(GetGValue(line_), GetGValue(accent_)),
                               mix(GetBValue(line_), GetBValue(accent_)))
                         : bg_);
            chartHits_.emplace_back(c, b.start);
            ++index;
        }
        return;
    }
    if (chart_ == 4) {
        auto max = *std::max_element(result_.weeklyHours.begin(), result_.weeklyHours.end());
        int cw = (r.right - r.left) / 24, ch = (r.bottom - r.top) / 7;
        for (int i = 0; i < 168; ++i) {
            auto v = result_.weeklyHours[i];
            double f = max ? static_cast<double>(v) / max : 0;
            auto mix = [&](BYTE a, BYTE b) { return static_cast<BYTE>(a + (b - a) * f); };
            COLORREF c =
                RGB(mix(GetRValue(line_), GetRValue(accent_)), mix(GetGValue(line_), GetGValue(accent_)),
                    mix(GetBValue(line_), GetBValue(accent_)));
            Fill(dc,
                 {r.left + (i % 24) * cw, r.top + (i / 24) * ch, r.left + (i % 24 + 1) * cw - 3,
                  r.top + (i / 24 + 1) * ch - 3},
                 c);
        }
        return;
    }
    const auto &data = chart_ == 5 ? result_.hours : result_.days;
    size_t n = data.size();
    if (!n)
        return;
    size_t stride = std::max<size_t>(1, (n + 119) / 120);
    std::vector<double> values;
    std::vector<long long> dates;
    double cumulative = 0;
    for (size_t i = 0; i < n; i += stride) {
        double v = 0;
        for (size_t j = i; j < std::min(i + stride, n); ++j)
            v += chart_ == 1   ? data[j].turns
                 : chart_ == 2 ? data[j].tasks
                               : static_cast<double>(data[j].tokens.totalTokens);
        cumulative += v;
        values.push_back(chart_ == 6 ? cumulative : v);
        dates.push_back(data[i].start);
    }
    double max = std::max(1.0, *std::max_element(values.begin(), values.end()));
    int count = static_cast<int>(values.size()), w = std::max(2L, (r.right - r.left) / count),
        height = r.bottom - r.top - 25;
    for (int i = 0; i < count; ++i) {
        int h = static_cast<int>(values[i] / max * (height - 8));
        int barWidth = std::min(w - 4, MulDiv(52, dpi_, 96)), x = r.left + i * w + w / 2;
        RECT b{x - barWidth / 2, r.top + height - h, x + barWidth / 2, r.top + height};
        if (chart_ == 6) {
            if (i > 0) {
                auto pen = CreatePen(PS_SOLID, 2, accent_);
                auto old = SelectObject(dc, pen);
                MoveToEx(dc, r.left + (i - 1) * w + w / 2,
                         r.top + height - static_cast<int>(values[i - 1] / max * (height - 8)), nullptr);
                LineTo(dc, r.left + i * w + w / 2, b.top);
                SelectObject(dc, old);
                DeleteObject(pen);
            }
        } else
            Fill(dc, b, accent_);
        RECT hit{r.left + i * w, r.top, r.left + (i + 1) * w, r.bottom};
        if (stride == 1)
            chartHits_.emplace_back(hit, activity::DayStart(dates[i]));
    }
    auto drawLine = [&](bool previous) {
        HPEN pen = CreatePen(PS_SOLID, 2, previous ? muted_ : RGB(224, 163, 77));
        auto old = SelectObject(dc, pen);
        bool started = false;
        for (int i = 0; i < count; ++i) {
            size_t j = i * stride;
            double v = -1;
            if (previous && j < result_.previousDays.size())
                v = static_cast<double>(result_.previousDays[j].tokens.totalTokens);
            if (!previous && j < result_.days.size())
                v = result_.days[j].movingAverage;
            if (v < 0) {
                started = false;
                continue;
            }
            int x = r.left + i * w + w / 2,
                y = r.top + height - static_cast<int>(std::min(1.0, v / max) * (height - 8));
            if (started)
                LineTo(dc, x, y);
            else
                MoveToEx(dc, x, y, nullptr);
            started = true;
        }
        SelectObject(dc, old);
        DeleteObject(pen);
    };
    if (chart_ == 0) {
        if (average_)
            drawLine(false);
        if (compare_ && result_.comparisonKnown)
            drawLine(true);
    }
    Label(dc, activity::Date(dates.front()), {r.left, r.bottom - 22, r.left + 150, r.bottom}, font_, muted_);
    Label(dc, activity::Date(dates.back()), {r.right - 150, r.bottom - 22, r.right, r.bottom}, font_, muted_,
          DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
}

void ActivityDetailsWindow::Worker(std::stop_token stop) {
    while (!stop.stop_requested()) {
        activity::Query query;
        codex_usage::LocalUsageSnapshot snapshot;
        std::uint64_t generation;
        std::stop_token queryToken;
        HWND target;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, stop, [&] { return requested_; });
            if (stop.stop_requested())
                return;
            query = workQuery_;
            snapshot = workSnapshot_;
            generation = generation_;
            target = workTarget_;
            queryToken = queryStop_.get_token();
            requested_ = false;
        }
        auto result = activity::Analyze(snapshot, query, queryToken);
        if (result.cancelled)
            continue;
        {
            std::lock_guard lock(mutex_);
            if (generation != generation_)
                continue;
            completed_ = std::move(result);
            completedGeneration_ = generation;
        }
        PostMessageW(target, kReady, 0, 0);
    }
}
void ActivityDetailsWindow::Request() {
    if (!hwnd_)
        return;
    {
        std::lock_guard lock(mutex_);
        queryStop_.request_stop();
        queryStop_ = std::stop_source{};
        workQuery_ = query_;
        workSnapshot_ = snapshot_;
        workTarget_ = hwnd_;
        ++generation_;
        requested_ = true;
        pending_ = true;
    }
    SetWindowTextW(Control(Status), Text(L"Updating analysis… previous results remain visible",
                                         L"正在更新分析… 保留上一份結果"));
    wake_.notify_all();
    Layout();
}
void ActivityDetailsWindow::SavePreferences() {
    if (settingsPath_.empty())
        return;
    auto write = [&](const wchar_t *k, std::uint64_t v) {
        WritePrivateProfileStringW(L"activity", k, std::to_wstring(v).c_str(), settingsPath_.c_str());
    };
    write(L"theme", theme_);
    write(L"period", query_.period == activity::Period::Custom ? 0 : static_cast<unsigned>(query_.period));
    write(L"compact", compact_);
    write(L"goal_enabled", showGoal_);
    write(L"daily_goal", dailyGoal_);
    write(L"page", page_);
    write(L"chart", chart_);
    write(L"card_order", cardOrder_);
    write(L"average", average_);
    write(L"compare", compare_);
}
void ActivityDetailsWindow::Command(int id, int notification) {
    if (id == IDOK) {
        HWND focus = GetFocus(); const int focusedId = GetDlgCtrlID(focus);
        if (focusedId == Table && page_ == 2) Command(Details, 0);
        else if (focusedId != IDOK && focusedId != IDCANCEL) {
            wchar_t className[32]{}; GetClassNameW(focus, className, 32);
            if (_wcsicmp(className, L"BUTTON") == 0) SendMessageW(focus, BM_CLICK, 0, 0);
        }
        return;
    }
    if (id == CardOrder && notification == CBN_SELCHANGE) {
        cardOrder_ = Selection(Control(CardOrder));
        SavePreferences();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (id == IDCANCEL) {
        ShowWindow(hwnd_, SW_HIDE);
        return;
    }
    if (id >= NavTrends && id <= NavPreferences) {
        page_ = id - NavTrends;
        for (int nav = NavTrends; nav <= NavPreferences; ++nav)
            InvalidateRect(Control(nav), nullptr, TRUE);
        chart_ = 0;
        pageIndex_ = 0;
        SendMessageW(Control(Chart), CB_RESETCONTENT, 0, 0);
        Populate();
        Layout();
        return;
    }
    if (id == Theme && notification == CBN_SELCHANGE) {
        theme_ = Selection(Control(Theme));
        ApplyTheme();
        SavePreferences();
        return;
    }
    if (id == Period && notification == CBN_SELCHANGE) {
        query_.period = static_cast<activity::Period>(Selection(Control(Period)));
        if (query_.period == activity::Period::Custom) {
            query_.start = PickerTime(Control(From), false);
            query_.end = PickerTime(Control(Until), true);
        }
        pageIndex_ = 0;
        Request();
        SavePreferences();
        return;
    }
    if (id == Source && notification == CBN_SELCHANGE) {
        query_.source = Selection(Control(Source)) - 1;
        pageIndex_ = 0;
        Request();
        return;
    }
    if (id == Chart && notification == CBN_SELCHANGE) {
        chart_ = Selection(Control(Chart));
        Populate();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (id == Average || id == Compare) {
        average_ = SendMessageW(Control(Average), BM_GETCHECK, 0, 0) == BST_CHECKED;
        compare_ = SendMessageW(Control(Compare), BM_GETCHECK, 0, 0) == BST_CHECKED;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if ((id == Search && notification == EN_CHANGE) ||
        ((id == Scope || id == Sort || id == PageSize) && notification == CBN_SELCHANGE)) {
        pageIndex_ = 0;
        search_ = Value(Control(Search));
        pageSize_ = Selection(Control(PageSize)) == 0 ? 25 : Selection(Control(PageSize)) == 2 ? 100 : 50;
        Populate();
        return;
    }
    if (id == Previous || id == Next) {
        int days = static_cast<int>(result_.days.size());
        if (!days)
            return;
        int shift = id == Previous ? -days : days;
        query_.period = activity::Period::Custom;
        query_.start = activity::ShiftDays(result_.start, shift);
        query_.end = activity::ShiftDays(result_.end, shift);
        if (query_.start > activity::DayStart(snapshot_.lastScanUnixSeconds))
            return;
        Select(Control(Period), 7);
        SetPicker(Control(From), query_.start);
        SetPicker(Control(Until), query_.end - 1);
        Request();
        return;
    }
    if (id == Today || id == Reset) {
        query_ = {};
        Select(Control(Period), 0);
        Select(Control(Source), 0);
        SetWindowTextW(Control(Search), L"");
        pageIndex_ = 0;
        Request();
        return;
    }
    if (id == Back || id == Forward) {
        pageIndex_ = std::max(0, pageIndex_ + (id == Back ? -1 : 1));
        Populate();
        return;
    }
    if (id == Copy) {
        auto text = activity::Summary(result_, chinese_) + L"\n" + activity::Date(result_.start, true) +
                    L" — " + activity::Date(result_.end - 1, true) + L"\nToken: " +
                    activity::Number(result_.tokens.totalTokens, chinese_, true) + L"\n" +
                    Text(L"Active tasks: ", L"活躍任務：") + Integer(result_.activeTasks) + L"\n" +
                    Text(L"User turns: ", L"使用者回合：") + Integer(result_.turns);
        CopyText(hwnd_, text);
        SetWindowTextW(Control(Status), Text(L"Values copied", L"已複製完整數值"));
        return;
    }
    if (id == Help) {
        ShowHelp();
        return;
    }
    if (id == Export) {
        ExportFile();
        return;
    }
    if (id == Scan || id == Rebuild || id == Cancel) {
        if (refresh_)
            refresh_(id == Scan ? 0 : id == Rebuild ? 1 : 2);
        SetWindowTextW(Control(Status), id == Cancel ? Text(L"Cancellation requested", L"已要求取消掃描")
                                                     : Text(L"Local scan requested", L"已要求掃描本機"));
        return;
    }
    if (id == Details) {
        int selected = ListView_GetNextItem(Control(Table), -1, LVNI_SELECTED);
        if (selected >= 0 && selected < static_cast<int>(visibleTaskIds_.size())) {
            query_.session = visibleTaskIds_[selected];
            page_ = 0;
            chart_ = 0;
            SendMessageW(Control(Chart), CB_RESETCONTENT, 0, 0);
            Request();
        }
        return;
    }
    if (id == SaveAlias) {
        EditTask(true);
        return;
    }
    if (id == SaveGoal || id == Compact) {
        auto value = Value(Control(Goal));
        wchar_t *end = nullptr;
        errno = 0;
        auto goal = _wcstoui64(value.c_str(), &end, 10);
        if (value.empty() || value[0] == L'-' || *end || errno == ERANGE) {
            MessageBoxW(hwnd_, Text(L"Enter a non-negative whole number.", L"請輸入有效的非負整數。"),
                        Text(L"Daily reference", L"每日參考上限"), MB_OK | MB_ICONINFORMATION);
            return;
        }
        showGoal_ = SendMessageW(Control(GoalEnabled), BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (showGoal_ && !goal) {
            MessageBoxW(hwnd_, Text(L"Set a reference greater than zero.", L"啟用時請設定大於 0 的參考值。"),
                        Text(L"Daily reference", L"每日參考上限"), MB_OK);
            return;
        }
        dailyGoal_ = goal;
        compact_ = SendMessageW(Control(Compact), BM_GETCHECK, 0, 0) == BST_CHECKED;
        SavePreferences();
        Populate();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
}
void ActivityDetailsWindow::Populate() {
    if (!Control(Table))
        return;
    int old = Selection(Control(Chart));
    if (SendMessageW(Control(Chart), CB_GETCOUNT, 0, 0) == 0) {
        if (page_ == 0)
            Choices(Control(Chart),
                    {Text(L"Daily tokens", L"每日 Token"), Text(L"Daily turns", L"每日回合"),
                     Text(L"Daily active tasks", L"每日活躍任務"), Text(L"Calendar heatmap", L"日曆熱度圖"),
                     Text(L"Weekday × hour", L"星期 × 小時"), Text(L"Hourly tokens", L"每小時 Token"),
                     Text(L"Cumulative tokens", L"累積 Token")});
        else if (page_ == 1)
            Choices(Control(Chart),
                    {Text(L"Token breakdown", L"Token 分類"), Text(L"Source distribution", L"來源分布"),
                     Text(L"Period statistics", L"期間統計"),
                     Text(L"Daily component ratios", L"每日分項比例"),
                     Text(L"Top 5 and remainder", L"前 5 名與其餘用量")});
        else if (page_ == 3)
            Choices(Control(Chart), {Text(L"Index diagnostics", L"索引診斷"),
                                     Text(L"Advanced capabilities", L"進階分析支援狀態")});
        old = 0;
    }
    if (old >= 0)
        Select(Control(Chart), chart_);
    HWND table = Control(Table);
    const int oldSelection = ListView_GetNextItem(table, -1, LVNI_SELECTED);
    const std::string selectedId =
        oldSelection >= 0 && oldSelection < static_cast<int>(visibleTaskIds_.size())
            ? visibleTaskIds_[oldSelection]
            : "";
    populating_ = true;
    SendMessageW(table, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(table);
    while (ListView_DeleteColumn(table, 0)) {
    }
    rows_.clear();
    visibleTaskIds_.clear();
    filteredTaskIds_.clear();
    std::vector<std::wstring> columns;
    auto num = [&](std::uint64_t v) { return activity::Number(v, chinese_, !compact_); };
    if (page_ == 0) {
        columns = {Text(L"Date / time", L"日期／時間"), L"Token", Text(L"User turns", L"使用者回合"),
                   Text(L"Active tasks", L"活躍任務"), Text(L"Data notes", L"資料說明")};
        if (chart_ == 4) {
            for (int d = 0; d < 7; ++d)
                for (int h = 0; h < 24; ++h)
                    rows_.push_back(
                        {std::to_wstring(d + 1) + L" / " + std::to_wstring(h) + Text(L":00", L" 時"),
                         num(result_.weeklyHours[d * 24 + h]), L"—", L"—",
                         Text(L"Mon = 1", L"1 代表星期一")});
        } else {
            auto &buckets = chart_ == 3 ? result_.calendar : chart_ == 5 ? result_.hours : result_.days;
            for (auto &b : buckets) {
                bool high = std::find(result_.highDays.begin(), result_.highDays.end(), b.start) !=
                            result_.highDays.end();
                std::wstring note =
                    b.initial ? Text(L"Includes initial counter", L"含首筆累積值")
                              : (!b.known && chart_ != 5 ? Text(L"Coverage uncertain", L"涵蓋狀態未確認")
                                 : high                  ? Text(L"Above recent baseline", L"高於近期基準")
                                                         : L"—");
                rows_.push_back({activity::Date(b.start, chart_ == 5), num(b.tokens.totalTokens),
                                 num(b.turns), num(b.tasks), note});
            }
        }
    } else if (page_ == 1) {
        columns = {Text(L"Metric", L"指標"), Text(L"Recorded value", L"已記錄數值"),
                   Text(L"Definition / coverage", L"定義／涵蓋")};
        auto &t = result_.tokens;
        if (chart_ == 0) {
            const wchar_t *names[] = {Text(L"Input", L"輸入"),
                                      Text(L"Cached input", L"快取輸入"),
                                      Text(L"Cache write", L"快取寫入"),
                                      Text(L"Output", L"輸出"),
                                      Text(L"Reasoning output", L"推理輸出"),
                                      Text(L"Total", L"總量")};
            std::uint64_t v[] = {t.inputTokens,  t.cachedInputTokens,     t.cacheWriteInputTokens,
                                 t.outputTokens, t.reasoningOutputTokens, t.totalTokens};
            for (int i = 0; i < 6; ++i)
                rows_.push_back(
                    {names[i], num(v[i]),
                     result_.validFields & (1u << i)
                         ? Text(L"Recorded field", L"已記錄欄位")
                         : Text(L"Known subtotal; some fields missing", L"已知小計；部分欄位缺漏")});
            rows_.push_back({Text(L"Cached / input", L"快取輸入／輸入"),
                             activity::Ratio(t.cachedInputTokens, t.inputTokens,
                                             (result_.validFields & 3) == 3, chinese_),
                             Text(L"Subset of input", L"輸入的分項")});
            rows_.push_back({Text(L"Reasoning / output", L"推理輸出／輸出"),
                             activity::Ratio(t.reasoningOutputTokens, t.outputTokens,
                                             (result_.validFields & 24) == 24, chinese_),
                             Text(L"Subset of output", L"輸出的分項")});
            rows_.push_back(
                {Text(L"Non-cached input", L"非快取輸入差額"),
                 (result_.validFields & 3) == 3 && t.inputTokens >= t.cachedInputTokens
                     ? num(t.inputTokens - t.cachedInputTokens)
                     : L"—",
                 Text(L"Input minus cached; cache write kept separate", L"輸入減快取；快取寫入另列")});
        } else if (chart_ == 1) {
            const wchar_t *names[] = {Text(L"Explicit top-level", L"已確認頂層"),
                                      Text(L"Non-top-level", L"非頂層"), Text(L"Unknown", L"未辨識來源")};
            for (int i = 0; i < 3; ++i)
                rows_.push_back({names[i], num(result_.sourceTokens[i]),
                                 Percent(result_.sourceTokens[i], t.totalTokens)});
        } else if (chart_ == 3) {
            columns = {Text(L"Date", L"日期"), Text(L"Cached / input", L"快取輸入／輸入"),
                       Text(L"Reasoning / output", L"推理輸出／輸出")};
            for (const auto &day : result_.days)
                rows_.push_back({activity::Date(day.start),
                                 activity::Ratio(day.tokens.cachedInputTokens, day.tokens.inputTokens,
                                                 day.hasTokens && (day.validFields & 3) == 3, chinese_),
                                 activity::Ratio(day.tokens.reasoningOutputTokens, day.tokens.outputTokens,
                                                 day.hasTokens && (day.validFields & 24) == 24, chinese_)});
        } else if (chart_ == 4) {
            std::uint64_t top = 0;
            for (size_t i = 0; i < std::min<size_t>(5, result_.tasks.size()); ++i) {
                const auto &task = result_.tasks[i];
                top += task.tokens.totalTokens;
                rows_.push_back({task.label, num(task.tokens.totalTokens),
                                 Percent(task.tokens.totalTokens, t.totalTokens)});
            }
            rows_.push_back({Text(L"Remaining sessions", L"其餘工作階段"),
                             num(t.totalTokens >= top ? t.totalTokens - top : 0),
                             Percent(t.totalTokens >= top ? t.totalTokens - top : 0, t.totalTokens)});
        } else {
            auto metric = [&](const wchar_t *en, const wchar_t *zh, std::wstring value,
                              const wchar_t *definition) {
                rows_.push_back({Text(en, zh), value, definition});
            };
            metric(L"Calendar-day turns", L"日曆日平均回合",
                   result_.days.empty()
                       ? L"—"
                       : Decimal(static_cast<long double>(result_.turns) / result_.days.size()),
                   Text(L"Includes zero-activity dates", L"包含未記錄活動日期"));
            metric(L"Active-day turns", L"活動日平均回合",
                   result_.activeDays ? Decimal(static_cast<long double>(result_.turns) / result_.activeDays)
                                      : L"—",
                   Text(L"Days with turns or tokens", L"有回合或 Token 的日期"));
            metric(L"Turns per active task", L"每活躍任務平均回合",
                   result_.activeTasks
                       ? Decimal(static_cast<long double>(result_.turns) / result_.activeTasks)
                       : L"—",
                   Text(L"Active top-level cohort", L"活躍頂層任務集合"));
            if (!result_.hours.empty()) {
                const auto peak = std::max_element(
                    result_.hours.begin(), result_.hours.end(),
                    [](const auto &a, const auto &b) { return a.tokens.totalTokens < b.tokens.totalTokens; });
                rows_.push_back({Text(L"Peak hour", L"最高用量小時"), num(peak->tokens.totalTokens),
                                 activity::Date(peak->start, true)});
            }
            metric(L"Calendar-day average", L"日曆日平均 Token",
                   result_.days.empty()
                       ? L"—"
                       : Decimal(static_cast<long double>(t.totalTokens) / result_.days.size()),
                   Text(L"Includes days with no recorded activity", L"包含未記錄活動的日期"));
            metric(L"Active-day average", L"活動日平均 Token",
                   result_.activeDays ? Decimal(static_cast<long double>(t.totalTokens) / result_.activeDays)
                                      : L"—",
                   Text(L"Days with turns or positive tokens", L"有回合或正 Token 增量的日期"));
            metric(L"Tokens per active task", L"每活躍任務平均 Token",
                   result_.activeTasks
                       ? Decimal(static_cast<long double>(result_.activeTaskTokens.totalTokens) /
                                 result_.activeTasks)
                       : L"—",
                   Text(L"Active top-level task cohort only", L"只計活躍頂層任務集合"));
            metric(L"Tokens / user turn", L"頂層 Token／使用者回合",
                   result_.turns
                       ? Decimal(static_cast<long double>(result_.topLevelTokens.totalTokens) / result_.turns)
                       : L"—",
                   Text(L"Period ratio, not request cost", L"期間比值，不是單次請求成本"));
            metric(L"Active days", L"有紀錄活動日", num(result_.activeDays), L"");
            metric(L"Longest streak", L"最長連續活動日", num(result_.longestStreak),
                   Text(L"Within selected period", L"限所選期間"));
            metric(L"Recent streak", L"最近連續活動日", num(result_.recentStreak), L"");
            metric(L"New tasks", L"新建立活躍任務", num(result_.newTasks), L"");
            metric(L"Returning tasks", L"回訪活躍任務", num(result_.returningTasks), L"");
            metric(L"Unknown creation", L"建立時間未知", num(result_.unknownAge), L"");
            metric(L"Token-only sessions", L"僅有 Token 的工作階段", num(result_.tokenOnly), L"");
            metric(L"Activity estimate", L"紀錄活動時間估計",
                   Decimal(result_.estimatedSeconds / 60.0L) + Text(L" min", L" 分鐘"),
                   Text(L"Union of 60-second event windows, not work time",
                        L"事件後 60 秒視窗聯集，不是工作時數"));
        }
    } else if (page_ == 2) {
        columns = {Text(L"Task / alias", L"任務／別名"),
                   L"Token",
                   Text(L"Turns", L"回合"),
                   Text(L"First activity", L"首次活動"),
                   Text(L"Last activity", L"最後活動"),
                   Text(L"Tags / origin", L"標籤／來源")};
        std::vector<const activity::Task *> tasks;
        for (auto &t : result_.tasks)
            tasks.push_back(&t);
        int sort = Selection(Control(Sort));
        if (sort == 1)
            std::stable_sort(tasks.begin(), tasks.end(), [](auto a, auto b) { return a->last > b->last; });
        if (sort == 2)
            std::stable_sort(tasks.begin(), tasks.end(), [](auto a, auto b) { return a->turns > b->turns; });
        std::vector<std::string> ids;
        for (auto t : tasks) {
            int scope = Selection(Control(Scope));
            if (scope == 1 && !t->topLevel)
                continue;
            if (scope == 2 && (!t->tokens.totalTokens || t->turns))
                continue;
            auto key = activity::AnonymousId(t->id);
            wchar_t alias[128]{}, tags[160]{};
            GetPrivateProfileStringW(L"activity_alias", key.c_str(), L"", alias, 128, settingsPath_.c_str());
            GetPrivateProfileStringW(L"activity_tags", key.c_str(), L"", tags, 160, settingsPath_.c_str());
            std::wstring label = alias[0] ? std::wstring(alias) + L" · " + key : key;
            std::wstring hay = label + L" " + tags, needle = search_;
            std::transform(hay.begin(), hay.end(), hay.begin(), towlower);
            std::transform(needle.begin(), needle.end(), needle.begin(), towlower);
            if (hay.find(needle) == std::wstring::npos)
                continue;
            rows_.push_back({label, num(t->tokens.totalTokens), num(t->turns), activity::Date(t->first, true),
                             activity::Date(t->last, true),
                             tags[0]          ? tags
                             : t->origin == 1 ? Text(L"Non-top-level", L"非頂層")
                             : t->origin == 0 ? Text(L"Top-level", L"頂層")
                                              : Text(L"Unknown origin", L"來源未辨識")});
            ids.push_back(t->id);
        }
        filteredTaskIds_ = ids;
        int total = static_cast<int>(rows_.size());
        pageIndex_ = std::min(pageIndex_, std::max(0, (total - 1) / pageSize_));
        int begin = pageIndex_ * pageSize_, end = std::min(begin + pageSize_, total);
        visibleTaskIds_.assign(ids.begin() + begin, ids.begin() + end);
        rows_ = std::vector<std::vector<std::wstring>>(rows_.begin() + begin, rows_.begin() + end);
        EnableWindow(Control(Back), pageIndex_ > 0);
        EnableWindow(Control(Forward), end < total);
    } else if (page_ == 3) {
        columns = {Text(L"Item", L"項目"), Text(L"Status / value", L"狀態／數值"),
                   Text(L"Explanation", L"說明")};
        if (chart_ == 0) {
            rows_ = {
                {Text(L"Coverage starts", L"紀錄起始"),
                 activity::Date(snapshot_.coverageStartUnixSeconds, true),
                 Text(L"Does not prove all history exists", L"不代表歷史紀錄完整保留")},
                {Text(L"Last event", L"最後事件"), activity::Date(snapshot_.lastEventUnixSeconds, true),
                 Text(L"Not a running-state indicator", L"不能據此判定正在執行")},
                {Text(L"Last scan", L"最後掃描"), activity::Date(snapshot_.lastScanUnixSeconds, true), L""},
                {Text(L"Files / read", L"檔案／本輪讀取"),
                 num(snapshot_.filesDiscovered) + L" / " + num(snapshot_.filesReadThisScan), L""},
                {Text(L"Bytes read", L"本輪讀取量"), activity::Bytes(snapshot_.bytesReadThisScan, chinese_),
                 L""},
                {Text(L"Scan duration", L"掃描耗時"),
                 num(snapshot_.scanMilliseconds) + Text(L" ms", L" 毫秒"), L""},
                {Text(L"Cache size", L"快取大小"), activity::Bytes(snapshot_.cacheBytes, chinese_), L""},
                {Text(L"Partial data", L"部分資料"),
                 snapshot_.partial ? Text(L"Yes", L"是")
                                   : Text(L"No detected read failure", L"未偵測到讀取失敗"),
                 Text(L"Deleted history cannot be inferred", L"無法推斷已刪除的歷史")}};
            if (snapshot_.activity) {
                auto &d = *snapshot_.activity;
                rows_.push_back({Text(L"Counter resets", L"累積計數重設"), num(d.resets), L""});
                rows_.push_back({Text(L"Duplicate events", L"重複事件"), num(d.duplicates), L""});
                rows_.push_back({Text(L"Invalid timestamps", L"無效時間戳"), num(d.invalidTimes), L""});
                rows_.push_back({Text(L"Malformed lines", L"格式錯誤列"), num(d.malformedLines), L""});
            }
        } else {
            const wchar_t *zh[] = {L"模型用量",     L"專案用量",     L"用戶端來源",   L"父子任務關聯",
                                   L"回合耗時",     L"活動時間估計", L"回合完成狀態", L"重試用量",
                                   L"工具呼叫",     L"上下文／壓縮", L"進行中狀態",   L"歸檔分布",
                                   L"單回合 Token", L"耗時並列比較"};
            const wchar_t *en[] = {
                L"Model usage",     L"Project usage",        L"Client origin", L"Parent / child attribution",
                L"Turn duration",   L"Activity estimate",    L"Turn outcomes", L"Retry usage",
                L"Tool calls",      L"Context / compaction", L"Running state", L"Archive distribution",
                L"Per-turn tokens", L"Duration comparisons"};
            for (int i = 0; i < 14; ++i)
                rows_.push_back({L"A" + std::to_wstring(47 + i) + L" · " + Text(en[i], zh[i]),
                                 i == 5 ? Text(L"Available as an estimate", L"提供估計值")
                                        : Text(L"Unavailable", L"尚未支援"),
                                 i == 5 ? Text(L"60-second event-window union", L"事件後 60 秒視窗聯集")
                                        : Text(L"Current index lacks verified attribution metadata",
                                               L"目前索引缺少可驗證的歸屬中繼資料")});
            auto groupRows = [&](int index, const auto &groups, bool project) {
                bool known = false;
                for (const auto &[key, count] : groups) {
                    known |= !key.empty();
                    std::wstring name = key.empty() ? Text(L"Unattributed", L"未辨識")
                                        : project   ? activity::AnonymousId(key)
                                                    : std::wstring(key.begin(), key.end());
                    rows_.push_back({Text(en[index], zh[index]) + std::wstring(L" · ") + name, num(count),
                                     index == 2 ? Text(L"Sessions with activity", L"有紀錄活動的工作階段")
                                                : Text(L"Token; matching consecutive context only",
                                                       L"Token；僅歸屬前後中繼資料一致的增量")});
                }
                rows_[index][1] = known ? Text(L"Partial coverage", L"部分支援")
                                        : Text(L"No attributed records", L"尚無可歸屬紀錄");
                rows_[index][2] =
                    Text(L"Unattributed records remain separate", L"未辨識紀錄另列，不推算補齊");
            };
            groupRows(0, result_.modelTokens, false);
            groupRows(1, result_.projectTokens, true);
            groupRows(2, result_.clientTasks, false);
            rows_[4][1] = result_.completedTurns ? Text(L"Available", L"已支援")
                                                 : Text(L"No duration records", L"尚無耗時紀錄");
            rows_[4][2] = L"P50 / P95: " + num(result_.durationP50) + L" / " + num(result_.durationP95) +
                          Text(L" ms", L" 毫秒");
            rows_[6][1] = num(result_.completedTurns);
            rows_[6][2] = Text(L"Recorded completions; explicit errors: ", L"已記錄完成事件；明確錯誤：") +
                          num(result_.failedTurns);
            rows_[11][1] = num(result_.archivedTasks);
            rows_[11][2] = Text(L"Observed archived files among active sessions",
                                L"本期有紀錄活動且曾觀察到歸檔檔案的工作階段");
        }
    }
    RECT tableRect{};
    GetClientRect(table, &tableRect);
    int totalWidth = std::max(600L, tableRect.right - 24);
    for (size_t c = 0; c < columns.size(); ++c) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = columns[c].data();
        col.cx = totalWidth / static_cast<int>(columns.size());
        ListView_InsertColumn(table, static_cast<int>(c), &col);
    }
    for (size_t i = 0; i < rows_.size(); ++i) {
        auto &row = rows_[i];
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = row[0].data();
        ListView_InsertItem(table, &item);
        for (size_t j = 1; j < row.size(); ++j)
            ListView_SetItemText(table, static_cast<int>(i), static_cast<int>(j), row[j].data());
    }
    if (!selectedId.empty()) {
        auto selected = std::find(visibleTaskIds_.begin(), visibleTaskIds_.end(), selectedId);
        if (selected != visibleTaskIds_.end())
            ListView_SetItemState(table, static_cast<int>(selected - visibleTaskIds_.begin()),
                                  LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    populating_ = false;
    SendMessageW(table, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(table, nullptr, TRUE);
    std::wstring status =
        result_.partial ? Text(L"Partial records", L"資料不完整") : Text(L"Local records", L"本機紀錄");
    status += L" · " + std::to_wstring(rows_.size()) + Text(L" rows", L" 列");
    if (page_ == 2)
        status += std::wstring(L" · ") + Text(L"Page ", L"第 ") + std::to_wstring(pageIndex_ + 1) +
                  Text(L"", L" 頁");
    if (result_.initial)
        status += Text(L" · Includes initial cumulative values", L" · 含首筆累積值");
    SetWindowTextW(Control(Status), status.c_str());
}
void ActivityDetailsWindow::EditTask(bool save) {
    int selected = ListView_GetNextItem(Control(Table), -1, LVNI_SELECTED);
    if (selected < 0 || selected >= static_cast<int>(visibleTaskIds_.size()))
        return;
    auto key = activity::AnonymousId(visibleTaskIds_[selected]);
    if (save) {
        auto alias = Value(Control(Alias)), tags = Value(Control(Tags));
        for (auto *s : {&alias, &tags}) {
            s->erase(std::remove_if(s->begin(), s->end(), [](wchar_t c) { return c < 32 || c == L'='; }),
                     s->end());
        }
        WritePrivateProfileStringW(L"activity_alias", key.c_str(), alias.c_str(), settingsPath_.c_str());
        WritePrivateProfileStringW(L"activity_tags", key.c_str(), tags.c_str(), settingsPath_.c_str());
        Populate();
    } else {
        wchar_t alias[128]{}, tags[160]{};
        GetPrivateProfileStringW(L"activity_alias", key.c_str(), L"", alias, 128, settingsPath_.c_str());
        GetPrivateProfileStringW(L"activity_tags", key.c_str(), L"", tags, 160, settingsPath_.c_str());
        SetWindowTextW(Control(Alias), alias);
        SetWindowTextW(Control(Tags), tags);
    }
}
void ActivityDetailsWindow::ShowHelp() {
    MessageBoxW(hwnd_,
                Text(L"Local recorded usage only.\n\nToken totals are positive changes "
                     L"in cumulative session counters. Cached input and reasoning are "
                     L"subsets and are not added again. First visible counters can "
                     L"include earlier history.\n\nActive tasks have a recorded user "
                     L"turn in the period. Subagent tokens count toward total usage, but "
                     L"their turns are not top-level user turns. Unknown sources remain "
                     L"separately labelled.\n\nDates follow the current Windows time "
                     L"zone. Missing records are not zero usage. Activity-time estimates "
                     L"use the union of 60-second windows after events, not working "
                     L"time.\n\nCharts have equivalent keyboard-accessible tables. "
                     L"Select a task and choose Analyze task to focus the analysis; "
                     L"Reset filters returns to all tasks.",
                     L"僅統計本機已記錄的活動。\n\nToken "
                     L"為工作階段累積計數的正向增量。快取輸入與推理輸出是分項，不再加回總"
                     L"量；第一筆可見累積值可能包含較早的歷史。\n\n活躍任務指本期有使用者"
                     L"回合的頂層任務。子代理 Token "
                     L"計入總量，子代理回合不計入頂層回合；來源未知時另列。\n\n日期依 "
                     L"Windows 目前時區。缺漏紀錄不代表零用量。活動時間使用事件後 60 "
                     L"秒視窗聯集估計，不是工作時數。\n\n圖表有可用鍵盤操作的等值表格。選"
                     L"取任務後按「分析此任務」聚焦分析，「重設篩選」可返回全部任務。"),
                Text(L"How statistics are calculated", L"統計如何計算"), MB_OK | MB_ICONINFORMATION);
}
void ActivityDetailsWindow::ExportFile() {
    if (!result_.available || pending_ || exportBusy_)
        return;
    int table = Selection(Control(ExportTable));
    bool json = Selection(Control(ExportFormat)) == 1;
    auto frozen = result_;
    if (table == 2 && page_ == 2) {
        std::erase_if(frozen.tasks, [&](const auto &task) {
            return std::find(filteredTaskIds_.begin(), filteredTaskIds_.end(), task.id) ==
                   filteredTaskIds_.end();
        });
    }
    auto preview = Text(L"Export the selected period with anonymous identifiers. "
                        L"Aliases and tags are not included.\n\n",
                        L"匯出所選期間與匿名代號，不包含別名或標籤。\n\n") +
                   activity::Date(result_.start) + L" — " + activity::Date(result_.end - 1);
    preview += L"\n" + std::wstring(Text(L"Rows: ", L"列數：")) +
               std::to_wstring(table == 2   ? frozen.tasks.size()
                               : table == 1 ? frozen.days.size()
                               : table == 3 ? frozen.quality.size()
                                            : 1);
    preview += std::wstring(L"\n") +
               Text(L"Fields: raw token components, turns, tasks, field validity, period, time zone and "
                    L"generation. Quality exports contain numeric diagnostics.",
                    L"欄位：Token "
                    L"原始分項、回合、任務、欄位完整性、期間、時區與快照時間；資料品質匯出包含數值診斷。");
    if (MessageBoxW(hwnd_, preview.c_str(), Text(L"Export preview", L"匯出預覽"),
                    MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
        return;
    wchar_t path[MAX_PATH] = L"activity";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd_;
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter = json ? L"JSON\0*.json\0\0" : L"CSV\0*.csv\0\0";
    dialog.lpstrDefExt = json ? L"json" : L"csv";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&dialog))
        return;
    if (exporter_.joinable())
        exporter_.join(); // Previous export has already sent its completion.
    exportBusy_ = true;
    EnableWindow(Control(Export), FALSE);
    SetWindowTextW(Control(Status), Text(L"Exporting snapshot…", L"正在匯出此份快照…"));
    exporter_ =
        std::jthread([frozen = std::move(frozen), table, json, zh = chinese_,
                      destination = std::filesystem::path(path), target = hwnd_](std::stop_token stop) {
            auto payload = activity::Export(frozen, table, json, zh);
            auto temporary = destination;
            temporary += L".codex-activity-" + std::to_wstring(GetTickCount64()) + L".tmp";
            if (stop.stop_requested())
                return;
            std::ofstream out(temporary, std::ios::binary);
            out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            out.close();
            bool success = out.good() && !stop.stop_requested() &&
                           MoveFileExW(temporary.c_str(), destination.c_str(),
                                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            if (!success) {
                std::error_code error;
                std::filesystem::remove(temporary, error);
            }
            PostMessageW(target, kExportReady, success ? 1 : 0, 0);
        });
}
