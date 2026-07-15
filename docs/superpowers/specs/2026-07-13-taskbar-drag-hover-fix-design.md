# 任務方格、懸浮球拖曳與可見群組 hover 修正規格

日期：2026-07-13  
範圍：CodexUsageMonitor 原生 Win32／Direct2D 小工具

## 1. 根因

### 1.1 任務模式

任務模式先由 72px 放大至 176px，但圓圈在工作列上過大；因此收斂為直徑 96px 的圓形，讓兩個用量區各佔上下半部，並以自適應文字內距維持可讀性。

### 1.2 懸浮球無法移動

`WM_LBUTTONDOWN` 先呼叫 `TryHandleControlClick`，而 `bubbleButtonRect_` 在懸浮球狀態涵蓋整個視窗，因此每一次懸浮球左鍵都直接切換展開狀態，沒有進入 `BeginDrag`。拖曳不可能開始。

### 1.3 垂直離開不收合

`UpdateHoverStateFromCursor` 使用 `GetWindowRect` 的完整矩形判定 `cursorInsideWindow`。展開群組的 Win32 視窗為包含 panel 與 bubble 的外接矩形，panel 下方、bubble 下方及兩者間距的透明像素仍被視為 inside；游標從懸浮球垂直離開時，可能落在這些不可見區域，導致 `HoverExpanded` 不回到 `Bubble`。

## 2. 設計

### 2.1 任務模式

- `kTaskbarWidgetLogicalDiameter` 設為 96px，寬高都由直徑驅動，實際尺寸仍依 DPI scaling。
- 保持單一液態玻璃圓形，不使用懸浮球、不因 hover 展開。
- 圓形內部分成上下兩個等高半圓區：上半部為 `5 hr`、下半部為 `Week`；不可用窗口顯示 `--`，可用窗口完整顯示剩餘百分比。
- 兩個半圓區採置中標籤／數值排版，以玻璃分隔線與狀態色點維持高對比，不讓數值被圓形邊緣裁切。

### 2.2 懸浮球點擊／拖曳

懸浮球左鍵改為「手勢延遲決策」：

- 左鍵按下時先建立 `Move` drag capture，但不立即切換狀態。
- 移動距離未超過 DPI-scaled 4px：放開時視為點擊，沿用既有 `Bubble ↔ PinnedExpanded` 狀態轉移。
- 任一方向超過 4px：視為拖曳，更新 `savedRect_` 與整個 panel／bubble 群組位置；放開時儲存位置，不觸發展開切換。
- `lockPosition_` 開啟時不建立拖曳 capture，懸浮球仍可直接點擊切換。
- panel 內既有拖曳、右上角 `×`、刷新控制與設定滑桿行為保留。

### 2.3 可見群組 hover

hover 命中改用 `GetCurrentGroupGeometry()` 的 `panel OR bubble` 可見區域，而不是 Win32 外接矩形：

- `Bubble` 只有游標在 bubble 時才展開。
- `HoverExpanded` 只有游標仍在 panel 或 bubble 時才維持；落在透明間距或透明外接矩形時立即收合。
- `PinnedExpanded` 不受游標離開影響。
- 同一個可見群組命中結果同時用於收合抑制判定，避免垂直離開後把透明區誤認為仍在 bubble。

## 3. 保護範圍

保留用量計算、帳號處理、API 重新整理、計時器、資料讀取、玻璃材質、字型、圖示、右鍵選單、透明度設定、既有面板位置設定與拖曳群組幾何；只修改任務模式尺寸與滑鼠手勢／hover 命中來源。

## 4. 驗證

- Presentation tests：直徑 96px 任務模式尺寸、4px 以上才算拖曳、panel／bubble 可見 union 與透明區排除。
- Source contract：直徑 96px、`IsDragGesture`、bubble pending click、可見群組命中與 `BeginDrag` 路徑。
- Debug／Release CTest、VS18 direct build、source contract。
- UI smoke：任務模式 96x96 圓形且上下兩半內容完整；完整／簡單模式可拖曳懸浮球；由懸浮球垂直上／下移出可見群組後收合；單擊仍可固定展開。
