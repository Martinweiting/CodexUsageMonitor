# 本機使用活動統計與增量索引規格

日期：2026-08-31  
範圍：CodexUsageMonitor 原生 Win32／Direct2D 小工具

## 1. 目標

在保留既有 5 小時／每週配額、reset credits、顯示模式與唯讀安全邊界的前提下，新增可驗證的本機 Codex 活動統計：

1. 今日 Token 使用量與本機紀錄累計 Token。
2. 今日活躍任務、本機任務總數。
3. 今日使用者回合、本機使用者回合總數。
4. 輸入、輸出、快取輸入、cache write 與推理輸出的 Token 分類。
5. 本機紀錄涵蓋時間、最後掃描時間、索引狀態與資料完整性提示。
6. 顯示目前用量端點已提供、但程式尚未呈現的可用狀態、credits 與 spend-control 摘要。

統計維持 local-first、唯讀及不蒐集對話內容。畫面不得把本機資料描述成跨裝置或帳戶建立以來的官方總量。

## 2. 顯示語意

### 2.1 今日

「今日」採 Windows 目前本地時區的日曆日，從本地 00:00 到現在。JSONL 的 ISO-8601 時間戳必須先轉成 Unix time，再依本地日界線歸類；不得直接以 session 目錄名稱或 UTC 日期判定。

### 2.2 Token

- `total_tokens` 是主要總量；輸入與輸出是其分項。
- `cached_input_tokens` 與 `cache_write_input_tokens` 是輸入 Token 的子集合，不可再次加到總量。
- `reasoning_output_tokens` 是輸出 Token 的分項，不可再次加到總量。
- 今日用量是每個 session 累積計數器在今日事件上的正向增量。
- 本機累計是所有可讀紀錄的去重增量總和。
- 若同一 session 的累積計數器下降，視為新的累積區段；該事件目前值成為新區段的第一筆增量。

UI 使用「今日 Token」及「本機累計 Token」，不得使用「帳戶總用量」。本機 Token 量與訂閱配額百分比是不同資料，不要求互相換算或對帳。

### 2.3 任務與回合

- 任務：唯一頂層 session／thread ID。
- 今日活躍任務：今日至少有一筆使用者訊息事件的唯一頂層 session ID。
- 使用者回合：新版 `response_item.message(role=user)` 或舊版 `event_msg.user_message`；以 session ID 與事件時間去重。
- `thread_source=user` 為頂層任務；`thread_source=subagent`、guardian 或 `source.subagent` 不計入任務／回合。
- 缺少新版來源欄位的舊紀錄，若來源不是 subagent／guardian，視為頂層任務。
- 子代理與 guardian 的 Token 仍計入 Token 使用量，因為它們確實使用模型資源。
- `session_index.jsonl` 可補足仍存在索引、但 session 檔案暫時不可讀的頂層任務 ID；不可用索引更新時間虛構今日回合。

## 3. 資料來源

### 3.1 遠端即時資料

沿用 `%CODEX_HOME%\auth.json` 或 `%USERPROFILE%\.codex\auth.json`，定時唯讀請求：

- `/backend-api/wham/usage`
- `/backend-api/wham/rate-limit-reset-credits`

除既有欄位外，解析下列 optional 資料：

- `rate_limit.allowed`
- `rate_limit.limit_reached`
- `rate_limit_reached_type`
- `credits.has_credits`
- `credits.unlimited`
- `credits.overage_limit_reached`
- `credits.balance`
- `credits.approx_local_messages`
- `credits.approx_cloud_messages`
- `spend_control.reached`
- `rate_limit_reset_credits.applicable_available_count`

所有新增欄位必須 null-safe；缺少或型別改變時只隱藏對應資訊，不得使 5 小時／每週配額整體失敗。

### 3.2 本機活動資料

讀取：

- `%CODEX_HOME%\sessions\**\*.jsonl`
- `%CODEX_HOME%\archived_sessions\**\*.jsonl`
- `%CODEX_HOME%\session_index.jsonl`

未設定 `CODEX_HOME` 時使用 `%USERPROFILE%\.codex`。只解析下列結構及其數字／識別欄位：

- `session_meta`
- `turn_context`（僅供來源上下文與向後相容）
- `event_msg.token_count`
- `event_msg.user_message`
- `response_item.message`（只讀取 `role=user` 與時間戳，不讀取或保存 content）

不得保存或顯示 user message、assistant message、reasoning、tool output、base instructions 或其他對話文字。

## 4. 增量索引與快取

### 4.1 快取位置與內容

快取位於 `%APPDATA%\CodexUsageMonitor\local-usage-cache-v1.tsv`，包含版本標頭及：

- 每個 session 檔案的穩定檔名、已處理 byte offset 與目前 session ID。
- 去重後的 session metadata。
- 去重後的使用者回合事件。
- 去重後的 Token 累積事件。
- session index 中的頂層任務 ID。

快取不得包含 prompt、response、reasoning、tool output、email、access token 或完整專案路徑。寫入採同目錄暫存檔加原子取代；格式錯誤或版本不符時忽略舊快取並重建。

### 4.2 初次與後續掃描

- 第一次啟用或快取無效時，在背景完整掃描一次。
- 後續每 15 秒檢查檔案大小，只讀取大於已保存 offset 的新增 bytes。
- 沒有檔案變更時不得重讀 JSONL 內容，也不得重寫快取。
- 檔案移到 `archived_sessions` 時以穩定檔名延續 checkpoint，避免重新計算。
- 檔案變小、被截斷或 checkpoint 不可信時，清空統計索引並完整重建，不能在舊彙整上疊加。
- 正在寫入且沒有換行結束的最後一列保留到下次，不解析、不前移 offset。
- 單一檔案無法讀取時繼續其他檔案，標示 partial，且不前移該檔案 checkpoint。

### 4.3 去重

- 複製到 fork 的歷史事件，使用 session ID、時間戳與累積 Token 欄位形成穩定鍵。
- 同一 session 的重複 `token_count` 即使時間戳不同，只要累積值未增加，其增量為零。
- 任務以 session ID 去重；使用者回合以 session ID 與事件時間去重。
- 聚合前按 session 及時間排序，避免檔案列舉順序影響結果。

## 5. 刷新與背景工作生命週期

- 遠端配額維持現有預設 60 秒，可選 1／3／5／10／30 分鐘。
- 本機索引每 15 秒檢查一次變更，只有新增資料時解析及保存。
- 啟動時立即各執行一次遠端刷新、本機索引與版本檢查。
- 手動刷新同時觸發遠端資料及本機增量索引。
- 每秒 UI timer 只更新倒數及重新繪製，不讀磁碟、不發網路請求。
- 版本檢查維持每 6 小時。
- 所有背景工作使用可停止、可 join 的 C++20 worker；視窗銷毀後不得再以裸指標結果投遞到已失效 HWND，也不得保留 detached thread。
- 同類工作尚未完成時跳過重複觸發。

## 6. 顯示設計

### 6.1 完整模式

完整模式新增「配額／活動」雙頁切換，沿用目前玻璃面板、Iansui／Quantico 字體與 650px 基礎高度。

活動頁包含：

- 六張摘要卡：今日 Token、本機累計 Token、今日任務、本機任務、今日回合、本機回合。
- Token 分類卡：今日與累計的 input、output、cached、cache write、reasoning。繁體中文頁面改為三欄兩列，避免中文單位造成擁擠。
- 帳戶狀態卡：允許／受限、credits balance／unlimited、預估本機／雲端訊息區間、spend control、適用 reset credits；資料缺少時顯示 `--` 或省略。
- 資料狀態卡：紀錄起始日、最後事件、最後掃描、索引中／partial／ready、檔案數與本輪讀取量。
- 活動頁刷新按鈕同時觸發遠端與本機刷新。

頁籤選擇保存於既有 `settings.ini`。切換頁籤不改變 hover／固定展開狀態。

### 6.2 簡單模式

保留兩張配額卡，新增一行「今日 Token · 今日任務」，並適度增加最小高度。首次索引尚未完成時顯示「本機統計建立中」。

### 6.3 任務模式與泡泡

任務模式圓形配額顯示與收合泡泡不增加文字，避免破壞可讀性與既有幾何。右鍵選單及刷新行為保持可用。

## 7. 錯誤與資料品質

- 遠端失敗不清除最後一次成功的本機統計；本機失敗也不使遠端配額失敗。
- 首次索引進行中顯示 indexing；已有快取時繼續顯示舊快照並標示 refreshing。
- partial 狀態必須可見，但仍顯示已成功解析的數字。
- 本機根目錄不存在時顯示 unavailable，不建立假零值。
- 所有數字使用 64-bit 無號計數並具備溢位保護。
- 英文介面的大數字採 K／M／B／T 緊湊格式；繁體中文介面改用符合中文習慣的萬／億／兆／京（例如 37.7 million 顯示為 3770 萬），詳細分項仍維持可辨識的數值。

## 8. 驗收要求

### 8.1 自動測試

- 遠端 JSON fixture 覆蓋新增 optional 欄位、null 欄位與錯誤型別。
- 本機 JSONL fixture 覆蓋今日／昨日增量、重複累積事件、累積計數器重設。
- fork fixture 證明複製的父歷史不重複計算、子代理 Token 會計入、子代理任務／回合不計入。
- cache fixture 證明第二次無變更掃描讀取 0 bytes，append 後只讀新增 bytes。
- archive move fixture 證明同檔名搬移不重算。
- 尾端不完整 JSON fixture 證明 offset 不越過未完成列。
- cache 內容不得出現測試 prompt／response marker。
- Presentation tests 覆蓋完整模式頁籤切換、緊湊數字格式與簡單模式高度。
- Source contract 繼續證明不存在 reset-credit consume 路徑，並要求受控 worker、本機快取名稱與 15 秒 timer 存在。

### 8.2 建置與執行驗證

- Debug CMake build 成功。
- Release CMake build 成功。
- CTest 全數通過。
- `tests/verify_source_contract.ps1` 通過。
- `git diff --check` 通過。
- 以實際本機 session 目錄完成首次掃描，記錄耗時、讀取 bytes、檔案數及非零統計；緊接的第二次掃描必須讀取 0 JSONL bytes。
- UI 煙霧檢查完整模式兩頁、簡單模式新增列、任務模式幾何與手動刷新；若無法執行互動畫面檢查，最終報告必須明確標示未驗證。

## 9. 非本次範圍

- 不宣稱跨裝置、已刪除或帳戶建立以來的官方總 Token／總對話。
- 不使用 OpenAI API organization Usage API，也不要求額外 API key。
- 不計算金額或「等值 API 成本」。
- 不保存／匯出對話內容、reasoning 或工具輸出。
- 不新增雲端同步、遙測、reset-credit 消耗或自動重設。
- 不在本次加入 7／30 日圖表、依模型／專案排行或 CSV／JSON 匯出；本次先建立可供後續功能使用的正確增量索引。
