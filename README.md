主要修改

1. 全域 Mapping Table

原本 SLC 與 TLC 各自維護 mapping table，改為由 FTL 維護一份 global mapping table。

2. I/O 指令語意

R | LBA | NLB

W | LBA | NLB | targetTier

T | LBA | NLB

M | LBA | NLB | targetTier

Q | tier

Read：由 global mapping 自動取得所在 tier。

Write：host 指定 target tier。

Trim：由 global mapping 自動取得所在 tier。

Migration：logical address 不變，只改變 physical tier。

Query：查詢指定 tier 在不觸發 GC 下可立即配置的 page 數量。

3. Cache

Cache key 只使用 global logical address，不再包含 tier。

尚未寫回 FTL 的 dirty data 會額外保留空間，避免 Q 高估可用空間。

4. Deferred Migration

收到 migration 指令時，資料不會立即搬移，而是先加入 migration list。

Migration 只在以下情況執行：

GC 選到的 victim block 中包含 pending migration data。

Pending migration list 達到設定上限。

Migration list 使用 FIFO list 保存執行順序。

採用以下規則：

重複 migration 採用較晚的那個

相同 target tier：不建立重複 entry

不同 target tier：更新 target

最新 target 已等於目前所在 tier：取消 pending migration

Migration enqueue 時會嘗試保留目標 tier 的空間。若空間不夠保留，則不加入任何 entry
