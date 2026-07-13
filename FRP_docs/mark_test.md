# free5GC / gtp5g 上行與下行 skb mark 實驗流程

更新日期：2026-06-23  
目的：驗證 gtp5g 能否依據 QoS Flow / QFI，將封包標記成 Linux `skb->mark` / `skb->priority`，並由 Linux TC 分流。

---

## 1. 實驗目標

本實驗要驗證兩個方向：

```text
上行：
UE → gNB → UPF → DN
gtp5g 解封裝後，依 Access PDR 的 pdr->qfi 標記 skb
再由 N6 / veth-upf egress TC 分流

下行：
DN → UPF → gNB → UE
gtp5g 封裝 GTP-U 前，依 Core PDR 的 pdr->qfi 標記 skb
再由 N3 egress TC 分流
```

目前 QFI / mark / TC class 對應：

| Flow type | QFI | skb->mark | skb->priority | TC class |
|---|---:|---:|---:|---|
| `10.200.0.2` flow | 2 | 2 | 2 | `1:10` |
| `10.200.0.3` flow | 3 | 3 | 3 | `1:20` |
| fallback / `0.0.0.0/0` flow | 4 | 4 | 4 | `1:30` |
| unmarked / default | 0 | 0 | 0 | `1:40` |

---

## 2. 拓樸與介面假設

請依你的實際環境調整介面名稱。

| 名稱 | 說明 | 本文件使用範例 |
|---|---|---|
| UE tunnel | UE 端 tunnel interface | `uesimtun0` |
| UPF N3 interface | UPF 連 gNB 的 GTP-U 介面 | `enp0s8` |
| UPF N6 / DN side interface | UPF 連 DN 的出口 | `veth-upf` |
| DN namespace | DN 測試環境 | `dn` |
| UE IP | UE tunnel IP | `10.60.0.1` |
| DN flow 1 | dedicated flow 1 | `10.200.0.2` |
| DN flow 2 | dedicated flow 2 | `10.200.0.3` |
| DN fallback | fallback flow | `10.200.0.4` |

建議先設定變數：

```bash
export N3_IF=enp0s8
export N6_IF=veth-upf
export DN_NS=dn
export UE_IP=10.60.0.1
export DN1=10.200.0.2
export DN2=10.200.0.3
export DN3=10.200.0.4
```

---

## 3. gtp5g 修正內容

### 3.1 skb label helper

```c
static void gtp5g_skb_label(u8 qfi, u32 *priority, u32 *mark)
{
    if (!qfi) {
        *priority = 0;
        *mark = 0;
        return;
    }

    *priority = qfi;
    *mark = qfi;
}
```

語意：

```text
QFI=0 → skb->priority=0, skb->mark=0
QFI=N → skb->priority=N, skb->mark=N
```

### 3.2 下行標記位置

下行在 `gtp5g_fwd_skb_ipv4()` 中，封裝 GTP-U header 前標記：

```c
gtp5g_apply_skb_label(skb, pdr->qfi);
gtp5g_push_header(skb, pktinfo);
```

下行意義：

```text
DN 送來普通 IP 封包
→ gtp5g 命中 Core PDR
→ 取得 pdr->qfi
→ 設定 skb->mark / skb->priority
→ push GTP-U header
→ 從 N3 interface 送出
→ N3 egress TC 依 mark 分流
```

### 3.3 上行標記位置

上行在解封裝後標記：

```c
gtp5g_apply_skb_label(skb, pdr->qfi ? pdr->qfi : qfi);
```

上行意義：

```text
N3 收到 GTP-U 封包
→ 解析原始 N3 QFI
→ 解封裝取得 inner IP packet
→ 命中 Access PDR
→ 優先使用 pdr->qfi 標記 skb
→ 若 pdr->qfi 為 0，fallback 使用 N3 原始 QFI
→ 往 N6 / veth-upf 送出
→ N6 egress TC 依 mark 分流
```

這對上行很重要，因為 UE / gNB 送來的 N3 uplink QFI 可能仍全部是 `QFI=1`。  
透過 `pdr->qfi ? pdr->qfi : qfi`，gtp5g 可以在 UPF 解封裝後，根據 Access PDR 重新做 flow-level 標記。

---

## 4. 重建與載入 gtp5g

在 gtp5g 目錄：

```bash
cd /home/ubuntu/gtp5g
make
```

確認已產生新的 kernel module：

```bash
ls -lh gtp5g.ko
modinfo ./gtp5g.ko | head
```

重新載入方式依你的環境而定。若目前 free5GC / UPF 正在使用 gtp5g，請先停止相關程序，再重新載入 module。

範例：

```bash
sudo rmmod gtp5g
sudo insmod /home/ubuntu/gtp5g/gtp5g.ko
lsmod | grep gtp5g
dmesg | tail -n 30
```

若你是透過 free5GC UPF 啟動時自動建立 `upfgtp`，重新載入 module 後需要重啟 UPF / free5GC 測試環境。

---

## 5. TC 設定：共用 class / filter 結構

本實驗使用 `skb->mark` 搭配 `tc fw filter` 分流。

對應關係：

```text
mark=2 → class 1:10
mark=3 → class 1:20
mark=4 → class 1:30
default → class 1:40
```

---

## 6. 上行測試流程

### 6.1 上行封包方向

```text
UE
→ gNB
→ UPF N3
→ gtp5g 解封裝
→ N6 / veth-upf
→ DN
```

因此上行要在 UPF 的 N6 / DN-side egress 介面觀察 TC，本文使用：

```bash
$N6_IF = veth-upf
```

### 6.2 設定上行 TC

清除舊設定：

```bash
sudo tc qdisc del dev $N6_IF root 2>/dev/null
```

建立 HTB root：

```bash
sudo tc qdisc add dev $N6_IF root handle 1: htb default 40
```

建立 class：

```bash
sudo tc class add dev $N6_IF parent 1: classid 1:10 htb rate 100mbit ceil 100mbit
sudo tc class add dev $N6_IF parent 1: classid 1:20 htb rate 100mbit ceil 100mbit
sudo tc class add dev $N6_IF parent 1: classid 1:30 htb rate 100mbit ceil 100mbit
sudo tc class add dev $N6_IF parent 1: classid 1:40 htb rate 100mbit ceil 100mbit
```

掛 child qdisc：

```bash
sudo tc qdisc add dev $N6_IF parent 1:10 handle 10: fq_codel
sudo tc qdisc add dev $N6_IF parent 1:20 handle 20: fq_codel
sudo tc qdisc add dev $N6_IF parent 1:30 handle 30: fq_codel
sudo tc qdisc add dev $N6_IF parent 1:40 handle 40: fq_codel
```

加入 fw mark filter：

```bash
sudo tc filter add dev $N6_IF parent 1: protocol ip prio 10 handle 0x2 fw flowid 1:10
sudo tc filter add dev $N6_IF parent 1: protocol ip prio 20 handle 0x3 fw flowid 1:20
sudo tc filter add dev $N6_IF parent 1: protocol ip prio 30 handle 0x4 fw flowid 1:30
```

確認設定：

```bash
sudo tc -s filter show dev $N6_IF parent 1:
sudo tc -s class show dev $N6_IF
```

### 6.3 啟動 DN iperf3 server

若 DN 使用 namespace：

```bash
sudo ip netns exec $DN_NS iperf3 -s -B $DN1 -p 5201
sudo ip netns exec $DN_NS iperf3 -s -B $DN2 -p 5202
```

若 DN 是另一台實體主機，請在 DN 主機執行：

```bash
iperf3 -s -B 10.200.0.2 -p 5201
iperf3 -s -B 10.200.0.3 -p 5202
```

### 6.4 上行測試 1：UE → 10.200.0.2

UE 端執行：

```bash
iperf3 -u -c 10.200.0.2 -p 5201 -b 10k -t 3 -B 10.60.0.1
```

觀察：

```bash
sudo tc -s class show dev $N6_IF
```

預期：

```text
class htb 1:10 增加
class htb 1:20 不增加
class htb 1:30 不增加
```

已觀察結果範例：

```text
class htb 1:10
 Sent 5813 bytes 22 pkt
```

結論：

```text
10.60.0.1 → 10.200.0.2
→ 命中 Access PDR
→ pdr->qfi=2
→ skb->mark=2
→ tc fw handle 0x2
→ class 1:10
```

### 6.5 上行測試 2：UE → 10.200.0.3

UE 端執行：

```bash
iperf3 -u -c 10.200.0.3 -p 5202 -b 10k -t 3 -B 10.60.0.1
```

觀察：

```bash
sudo tc -s class show dev $N6_IF
```

預期：

```text
class htb 1:20 增加
class htb 1:10 不增加或僅維持原累積值
class htb 1:30 不增加
```

已觀察結果範例：

```text
class htb 1:20
 Sent 5811 bytes 22 pkt
```

結論：

```text
10.60.0.1 → 10.200.0.3
→ 命中 Access PDR
→ pdr->qfi=3
→ skb->mark=3
→ tc fw handle 0x3
→ class 1:20
```

### 6.6 上行測試 3：UE → 10.200.0.4 fallback

UE 端執行：

```bash
ping -I uesimtun0 10.200.0.4 -c 5
```

觀察：

```bash
sudo tc -s class show dev $N6_IF
```

預期：

```text
class htb 1:30 增加
```

已觀察結果範例：

```text
class htb 1:30
 Sent 490 bytes 5 pkt
```

結論：

```text
10.60.0.1 → 10.200.0.4
→ 未命中 10.200.0.2/32
→ 未命中 10.200.0.3/32
→ 命中 0.0.0.0/0 fallback Access PDR
→ pdr->qfi=4
→ skb->mark=4
→ tc fw handle 0x4
→ class 1:30
```

### 6.7 上行測試結論

目前已完成的上行結果：

| 上行 flow | 預期 QFI / mark | 實際 TC class | 結果 |
|---|---:|---|---|
| `10.60.0.1 → 10.200.0.2` | 2 | `1:10` | 成功 |
| `10.60.0.1 → 10.200.0.3` | 3 | `1:20` | 成功 |
| `10.60.0.1 → 10.200.0.4` | 4 | `1:30` | 成功 |

可下結論：

```text
上行解封裝後，gtp5g 已能依據 Access PDR 的 flow-specific QFI，
將不同 uplink flow 標記成不同 skb->mark / skb->priority，
並成功讓 Linux TC 分流。
```

注意：  
這證明的是 UPF / gtp5g 解封裝後的 Linux data-plane 標記與分流成功。  
它不一定代表 N3 uplink 原始 GTP-U 封包本身已經帶不同 QFI。

---

## 7. 下行測試流程

### 7.1 下行封包方向

```text
DN
→ UPF N6 / veth-upf
→ gtp5g 命中 Core PDR
→ gtp5g_apply_skb_label(skb, pdr->qfi)
→ gtp5g_push_header()
→ UPF N3 interface
→ gNB
→ UE
```

因此下行要在 UPF 的 N3 egress 介面觀察 TC，本文使用：

```bash
$N3_IF = enp0s8
```

重點：

```text
下行不要看 veth-upf 的 egress TC。
下行 mark 是在封裝 GTP-U 前設定，但最後會跟著同一個 skb 從 N3 送出。
所以要在 N3 interface，例如 enp0s8，看 TC class counter。
```

### 7.2 設定下行 N3 TC

1. 清除舊設定
2. 建立 HTB root
3. 建立 class
4. 掛 child qdisc
5. 加入 fw mark filter
6. 確認設定
```bash
sudo tc qdisc del dev $N3_IF root 2>/dev/null

sudo tc qdisc add dev $N3_IF root handle 1: htb default 40

sudo tc class add dev $N3_IF parent 1: classid 1:10 htb rate 100mbit ceil 100mbit
sudo tc class add dev $N3_IF parent 1: classid 1:20 htb rate 100mbit ceil 100mbit
sudo tc class add dev $N3_IF parent 1: classid 1:30 htb rate 100mbit ceil 100mbit
sudo tc class add dev $N3_IF parent 1: classid 1:40 htb rate 100mbit ceil 100mbit

sudo tc qdisc add dev $N3_IF parent 1:10 handle 10: fq_codel
sudo tc qdisc add dev $N3_IF parent 1:20 handle 20: fq_codel
sudo tc qdisc add dev $N3_IF parent 1:30 handle 30: fq_codel
sudo tc qdisc add dev $N3_IF parent 1:40 handle 40: fq_codel

sudo tc filter add dev $N3_IF parent 1: protocol ip prio 10 handle 0x2 fw flowid 1:10
sudo tc filter add dev $N3_IF parent 1: protocol ip prio 20 handle 0x3 fw flowid 1:20
sudo tc filter add dev $N3_IF parent 1: protocol ip prio 30 handle 0x4 fw flowid 1:30

sudo tc -s filter show dev $N3_IF parent 1:
sudo tc -s class show dev $N3_IF
```

### 7.3 同時抓 N3 pcap

建議下行 TC 測試時同步抓 N3 GTP-U pcap，用來比對：

```text
N3 GTP-U PDU Session Container QFI
與
TC class counter
```

抓封包：

```bash
sudo tcpdump -i $N3_IF -nn -s 0 -w n3_downlink_mark_test.pcap udp port 2152
```

Wireshark 中確認：

```text
GPRS Tunneling Protocol
→ Extension Header: PDU Session Container
→ PDU Type: DL PDU SESSION INFORMATION
→ QoS Flow Identifier (QFI)
```

### 7.4 下行測試 1：10.200.0.2 → UE

使用 iperf3 reverse mode。  
UE 作為 iperf3 client，但加上 `-R` 後，主要資料流方向是 DN → UE。

UE 端執行：

```bash
iperf3 -u -c 10.200.0.2 -p 5201 -R -b 10k -t 3 -B 10.60.0.1
```

觀察 N3 TC：

```bash
sudo tc -s class show dev $N3_IF
```

預期：

```text
class htb 1:10 增加
```

預期封包語意：

```text
10.200.0.2 → 10.60.0.1
→ 命中 Core PDR
→ pdr->qfi=2
→ skb->mark=2
→ gtp5g_push_header()
→ N3 egress tc fw handle 0x2
→ class 1:10
```

N3 pcap 預期：

```text
Inner IP: 10.200.0.2 → 10.60.0.1
PDU Type: DL PDU SESSION INFORMATION
QFI=2
```

### 7.5 下行測試 2：10.200.0.3 → UE

UE 端執行：

```bash
iperf3 -u -c 10.200.0.3 -p 5202 -R -b 10k -t 3 -B 10.60.0.1
```

觀察 N3 TC：

```bash
sudo tc -s class show dev $N3_IF
```

預期：

```text
class htb 1:20 增加
```

預期封包語意：

```text
10.200.0.3 → 10.60.0.1
→ 命中 Core PDR
→ pdr->qfi=3
→ skb->mark=3
→ gtp5g_push_header()
→ N3 egress tc fw handle 0x3
→ class 1:20
```

N3 pcap 預期：

```text
Inner IP: 10.200.0.3 → 10.60.0.1
PDU Type: DL PDU SESSION INFORMATION
QFI=3
```

### 7.6 下行測試 3：fallback flow → UE

可使用 ping 測試 ICMP Echo Reply 的下行方向。

UE 端執行：

```bash
ping -I uesimtun0 10.200.0.4 -c 5
```

這個指令會產生：

```text
上行：10.60.0.1 → 10.200.0.4，ICMP Echo Request
下行：10.200.0.4 → 10.60.0.1，ICMP Echo Reply
```

此處下行要觀察 N3 interface：

```bash
sudo tc -s class show dev $N3_IF
```

預期：

```text
class htb 1:30 增加
```

預期封包語意：

```text
10.200.0.4 → 10.60.0.1
→ 未命中 10.200.0.2/32 Core PDR
→ 未命中 10.200.0.3/32 Core PDR
→ 命中 0.0.0.0/0 fallback Core PDR
→ pdr->qfi=4
→ skb->mark=4
→ gtp5g_push_header()
→ N3 egress tc fw handle 0x4
→ class 1:30
```

N3 pcap 預期：

```text
Inner IP: 10.200.0.4 → 10.60.0.1
PDU Type: DL PDU SESSION INFORMATION
QFI=4
```

---

## 8. 建議的觀察方式

### 8.1 每次測試前重設 counter

`tc` counter 會累積。若要避免混淆，建議每條 flow 測試前重設 qdisc。

上行重設：

```bash
sudo tc qdisc del dev $N6_IF root 2>/dev/null
# 重新套用第 6.2 節 TC 設定
```

下行重設：

```bash
sudo tc qdisc del dev $N3_IF root 2>/dev/null
# 重新套用第 7.2 節 TC 設定
```

或者使用前後差值：

```bash
sudo tc -s class show dev $N3_IF
# 執行測試
sudo tc -s class show dev $N3_IF
# 比較 Sent bytes / pkt 增量
```

### 8.2 使用 watch 觀察

上行：

```bash
watch -n 1 "tc -s class show dev $N6_IF"
```

下行：

```bash
watch -n 1 "tc -s class show dev $N3_IF"
```

### 8.3 filter counter 也要看

除了 class，也可看 filter 命中：

上行：

```bash
sudo tc -s filter show dev $N6_IF parent 1:
```

下行：

```bash
sudo tc -s filter show dev $N3_IF parent 1:
```

若 `filter handle 0x2 / 0x3 / 0x4` 的 packet counter 增加，表示 `skb->mark` 有被 `fw filter` 正確匹配。

---

## 9. 常見誤解

### 9.1 `tc -s qdisc show` 不適合看三條 flow

`tc -s qdisc show dev <iface>` 主要顯示 qdisc 總體統計，不適合判斷三條 flow 各自命中哪個 class。

建議使用：

```bash
tc -s class show dev <iface>
tc -s filter show dev <iface> parent 1:
```

### 9.2 fq_codel 內部 class 不代表 QFI class

你可能會看到：

```text
class fq_codel 10:377 parent 10:
class fq_codel 10:3a7 parent 10:
```

這些是 `fq_codel` 內部 hash 出來的小 flow bucket，不是你手動定義的 QFI class。  
判斷 QFI 分流時，主要看：

```text
class htb 1:10
class htb 1:20
class htb 1:30
class htb 1:40
```

### 9.3 下行要看 N3 egress，不是 N6 egress

下行主要資料流是：

```text
DN → UPF → gNB → UE
```

gtp5g 在封裝前設定 `skb->mark`，封裝後從 N3 interface 送出。  
因此下行 TC 應該掛在 N3 interface，例如：

```bash
enp0s8
```

不是掛在：

```bash
veth-upf
```

### 9.4 N3 pcap 看得到 QFI，但看不到 skb->mark

Wireshark 可以看到：

```text
GTP-U PDU Session Container QFI
```

但看不到：

```text
skb->mark
skb->priority
```

因為 `skb->mark` / `skb->priority` 是 Linux kernel metadata，不是封包內容。  
因此需要用 TC counter 驗證 mark 是否生效。

---

## 10. 最終應得到的結果表

### 10.1 上行結果

| 測試指令 | 方向 | 預期 mark | 預期 class | 狀態 |
|---|---|---:|---|---|
| `iperf3 -u -c 10.200.0.2 -p 5201 -b 10k -t 3 -B 10.60.0.1` | UE → `10.200.0.2` | 2 | `1:10` | 已成功 |
| `iperf3 -u -c 10.200.0.3 -p 5202 -b 10k -t 3 -B 10.60.0.1` | UE → `10.200.0.3` | 3 | `1:20` | 已成功 |
| `ping -I uesimtun0 10.200.0.4 -c 5` | UE → `10.200.0.4` | 4 | `1:30` | 已成功 |

### 10.2 下行預期結果

| 測試指令 | 方向 | 預期 mark | 預期 class | N3 QFI |
|---|---|---:|---|---:|
| `iperf3 -u -c 10.200.0.2 -p 5201 -R -b 10k -t 3 -B 10.60.0.1` | `10.200.0.2` → UE | 2 | `1:10` | 2 |
| `iperf3 -u -c 10.200.0.3 -p 5202 -R -b 10k -t 3 -B 10.60.0.1` | `10.200.0.3` → UE | 3 | `1:20` | 3 |
| `ping -I uesimtun0 10.200.0.4 -c 5` | `10.200.0.4` → UE Echo Reply | 4 | `1:30` | 4 |

---

## 11. 可寫入論文的結論

英文：

```text
After the gtp5g modification, the UPF is able to translate QoS Flow information into Linux skb metadata. For uplink traffic, gtp5g marks the decapsulated packets according to the matched Access PDR's QFI, with fallback to the original QFI in the N3 PDU Session Container. For downlink traffic, gtp5g marks the packet according to the matched Core PDR's QFI before pushing the GTP-U header. The TC class counters confirm that traffic associated with QFI 2, QFI 3, and QFI 4 is mapped to different Linux traffic classes. This demonstrates that 5G QoS Flow-level information can be propagated to the Linux data plane for traffic classification and scheduling.
```

中文：

```text
修正後，gtp5g 已能將 QoS Flow 的 QFI 轉換成 Linux skb metadata。上行封包在解封裝後，依據命中的 Access PDR QFI 設定 skb->mark / skb->priority；若 Access PDR 沒有 QFI，則 fallback 使用 N3 PDU Session Container 原始 QFI。下行封包則在封裝 GTP-U header 前，依據命中的 Core PDR QFI 設定 skb->mark / skb->priority。TC class counter 顯示 QFI 2、QFI 3、QFI 4 能分別進入不同 Linux traffic class，證明 5G QoS Flow-level 資訊已能落地到 Linux data-plane 分類與排程。
```
