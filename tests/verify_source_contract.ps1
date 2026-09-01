$files = @(
    'src/AppBarWindow.h',
    'src/AppBarWindow.cpp',
    'src/WidgetPresentation.h',
    'src/WidgetPresentation.cpp',
    'src/CodexUsageFetcher.h',
    'src/CodexUsageFetcher.cpp'
    'src/LocalUsageStats.h'
    'src/LocalUsageStats.cpp'
)
$content = ($files | ForEach-Object { Get-Content -Raw -Encoding UTF8 $_ }) -join "`n"
$forbidden = @(
    'ConsumeRateLimitResetCredit',
    'ConsumeResetCreditResult',
    'HttpPostConsumeRateLimitResetCredit',
    'rate-limit-reset-credits/consume',
    'kResetCreditConsumedMessage',
    'kResetConfirmTimerId',
    'RequestConsumeResetCredit',
    'resetCreditConfirmStep_',
    'CreateRedeemRequestId'
    '.detach()'
    'constexpr DWORD extendedWindowStyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW'
)
$found = @($forbidden | Where-Object { $content.Contains($_) })
if ($found.Count -gt 0) {
    Write-Error ('Forbidden reset-consumption symbols remain: ' + ($found -join ', '))
    exit 1
}
$required = @(
    'assets\\icons\\codex-liquid.png',
    'assets\\icons\\undo.png',
    'assets\\fonts\\Iansui-Regular.ttf',
    'assets\\fonts\\Quantico-Regular.ttf',
    'Iansui',
    'Quantico',
    'Transparency',
    ([string]::Concat([char]0x900F, [char]0x660E, [char]0x5EA6)),
    'kTaskbarDefaultWidgetWidth = codex_widget::kTaskbarWidgetLogicalWidth',
    'constexpr int kTaskbarWidgetLogicalDiameter = 96',
    'constexpr int kTaskbarWidgetLogicalWidth = kTaskbarWidgetLogicalDiameter',
    'constexpr int kTaskbarWidgetLogicalHeight = kTaskbarWidgetLogicalDiameter',
    'kHoverPollTimerId',
    'WM_RBUTTONUP',
    'WM_NCRBUTTONUP',
    'HoverStateForCursor',
    'IsDragGesture',
    'bubbleClickPending_',
    'dragMoved_',
    'ActivateBubbleClick',
    'IsPointInsideWidgetGroup(group',
    'lastRefreshCompletedUnixSeconds_',
    'lastRefreshSucceeded_',
    'GetRefreshStatusText',
    'SetForegroundWindow(hwnd_)',
    'drawVectorIcon',
    'if (taskbarMode_ && !settingsOpen_)',
    'const bool usesFloatingBubble = !taskbarMode_',
    'SetWindowRgn(hwnd_',
    'const int split = half.left + (RectWidth(half) * 35) / 100'
    'local-usage-cache-v1.tsv'
    'kLocalUsageRefreshMilliseconds = 15 * 1000'
    'std::jthread'
    'request_stop()'
    'RequestLocalUsageRefresh'
    'FullPageAfterTabClick'
    'CodexUsageMonitorOwner'
    'WM_MOUSEACTIVATE'
    'MA_ACTIVATE'
    'fillInteractiveHitTarget'
    'FormatTraditionalChineseCount'
    'Today tokens'
    ([string]::Concat([char]0x4ECA, [char]0x65E5, ' Token'))
)
$missing = @($required | Where-Object { -not $content.Contains($_) })
if ($missing.Count -gt 0) {
    Write-Error ('Required visual asset/font symbols are missing: ' + ($missing -join ', '))
    exit 1
}
Write-Output 'Reset-consumption source contract passed'
