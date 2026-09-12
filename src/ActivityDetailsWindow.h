#pragma once
#include "ActivityAnalytics.h"
#include <Windows.h>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

class ActivityDetailsWindow {
  public:
    ActivityDetailsWindow();
    ~ActivityDetailsWindow();
    void Open(HWND owner, bool chinese, const codex_usage::LocalUsageSnapshot &snapshot,
              const std::wstring &settingsPath, std::function<void(int)> refresh);
    void Update(const codex_usage::LocalUsageSnapshot &snapshot, bool chinese);
    bool Translate(MSG &message);
    void Close();

  private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);
    void CreateControls();
    void Layout();
    void Paint(HDC);
    void DrawChart(HDC, RECT);
    void Command(int id, int notification);
    void Request();
    void Populate();
    void ApplyTheme();
    void SavePreferences();
    void ExportFile();
    void ShowHelp();
    void EditTask(bool save);
    void Worker(std::stop_token);
    const wchar_t *Text(const wchar_t *en, const wchar_t *zh) const { return chinese_ ? zh : en; }
    HWND Control(int id) const { return GetDlgItem(hwnd_, id); }
    HWND hwnd_ = nullptr;
    bool chinese_ = true, pending_ = false, average_ = true, compare_ = false, compact_ = false,
         showGoal_ = false;
    int page_ = 0, theme_ = 0, chart_ = 0, pageIndex_ = 0, pageSize_ = 50;
    unsigned dpi_ = 96;
    HFONT font_ = nullptr, numberFont_ = nullptr, titleFont_ = nullptr;
    HBRUSH background_ = nullptr, cardBrush_ = nullptr;
    COLORREF bg_ = 0, card_ = 0, ink_ = 0, muted_ = 0, accent_ = 0, line_ = 0;
    RECT chartRect_{};
    std::vector<std::pair<RECT, long long>> chartHits_;
    std::wstring settingsPath_, search_;
    std::uint64_t dailyGoal_ = 0;
    std::vector<std::string> visibleTaskIds_;
    std::vector<std::string> filteredTaskIds_;
    bool populating_ = false;
    int cardOrder_ = 0;
    int scrollOffset_ = 0;
    std::vector<std::vector<std::wstring>> rows_;
    codex_usage::LocalUsageSnapshot snapshot_;
    activity::Query query_;
    activity::Result result_;
    std::function<void(int)> refresh_;
    std::jthread worker_;
    std::jthread exporter_;
    bool exportBusy_ = false;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    bool requested_ = false;
    std::uint64_t generation_ = 0, completedGeneration_ = 0;
    activity::Query workQuery_;
    codex_usage::LocalUsageSnapshot workSnapshot_;
    std::optional<activity::Result> completed_;
    std::stop_source queryStop_;
    HWND workTarget_ = nullptr;
};
