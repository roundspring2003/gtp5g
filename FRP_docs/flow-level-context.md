# free5GC / gtp5g Flow-Level QoS 研究上下文整理

更新日期：2026-06-23  
主題：由 Network Slice-Level 驗證，轉向同一 PDU Session 內的 QoS Flow-Level 差異化驗證

---

## 1. 研究方向調整

原本研究主軸是：

> 驗證 free5GC / gtp5g 在 Network Slicing 中，是否只完成 control-plane slicing，而尚未真正將 slice / QoS Flow 對應到 Linux queue、CPU、IRQ、softirq 等 data-plane / compute-plane 資源隔離。

原先實驗設計是：

```text
一個 UE
├── PDU Session 1 → Slice 1 / eMBB
└── PDU Session 2 → Slice 2 / URLLC-like
```

目前考慮調整成：

```text
一個 UE
└── 一個 Slice / 一個 PDU Session
    ├── QoS Flow 1 → QFI 1 / 5QI 9
    ├── QoS Flow 2 → QFI x / 5QI 8
    └── QoS Flow 3 → QFI y / 5QI 7
```

因此研究定位從：

```text
cross-slice differentiation / slice-level isolation
```

轉向：

```text
QoS Flow-level differentiation inside one PDU Session
```

較精確的論文描述應為：

> 本研究聚焦於同一 PDU Session 內的 QoS Flow-level differentiation，驗證 free5GC / gtp5g 是否能將 QFI / 5QI 等 5G QoS metadata 落地到 Linux traffic control 可執行的 priority scheduling。

---

## 2. 重要概念釐清

### 2.1 Slice、PDU Session、TEID、QFI 的關係

```text
S-NSSAI = Slice identity
PDU Session = UE 與 DN 之間的一條資料會話
TEID = GTP-U tunnel identifier
QFI = PDU Session 內部的 QoS Flow Identifier
5QI = QoS Flow 的 QoS profile / QoS characteristic
```

重點：

```text
QFI 不是 slice ID
TEID 也不是 slice ID
TEID 可作為 data-plane key 反查 PDU Session context
S-NSSAI 才是真正的 slice identity
```

在 flow-level 實驗中：

```text
同一個 PDU Session
→ 通常同一組 TEID
→ 用不同 QFI 區分 QoS Flow
```

因此實驗目標變成：

```text
同一 TEID 下，是否能觀察到不同 QFI
```

例如：

```text
TEID=2, QFI=1 → default flow / 5QI=9
TEID=2, QFI=2 → background flow / 5QI=8
TEID=2, QFI=3 → low-latency flow / 5QI=7
```

---

## 3. 上行與下行封包路徑

### 3.1 上行 UE → DN

```text
UE
→ UERANSIM tunnel
→ gNB
→ GTP-U 封裝
→ UPF N3 實體網卡
→ gtp5g 依 TEID / QFI 解封裝
→ upfgtp 虛擬介面
→ Linux routing
→ N6 / veth-upf
→ DN namespace / DN server
```

上行 N3 封包長相：

```text
Outer IP: gNB N3 IP → UPF N3 IP
UDP 2152
GTP-U Header
├── TEID
└── PDU Session Container
    └── QFI
Inner IP: UE IP → DN IP
```

目前觀察：

```text
上行實際封包仍為 QFI=1
```

### 3.2 下行 DN → UE

```text
DN server
→ N6 / veth-upf
→ Linux routing
→ upfgtp
→ gtp5g 依 PDR / FAR / QER 封裝
→ UPF N3 實體網卡
→ gNB
→ UE
```

下行 N3 封包長相：

```text
Outer IP: UPF N3 IP → gNB N3 IP
UDP 2152
GTP-U Header
├── Downlink TEID
└── PDU Session Container
    └── QFI
Inner IP: DN IP → UE IP
```

目前觀察：

```text
Inner IP: 10.200.0.3 → 10.60.0.1
PDU Type: DL PDU SESSION INFORMATION
QFI=1
```

代表下行也仍使用 default QFI=1。

---

## 4. gtp5g 與 UPF 的角色

整體功能上可以說是 UPF 在處理 user-plane，但實際逐封包封裝 / 解封裝是在 Linux kernel 的 gtp5g 模組中完成。

```text
free5GC UPF user-space
→ 接收 PFCP
→ 將 PDR / FAR / QER 寫入 gtp5g

gtp5g kernel module
→ 實際處理 GTP-U encapsulation / decapsulation
→ 查 TEID / PDR / FAR / QER
→ 產生或解析 PDU Session Container QFI
```

`upfgtp` 是 gtp5g 建立的虛擬網路介面，用來把 GTP-U tunnel 接到 Linux networking stack。

```text
下行：
Linux routing → upfgtp → gtp5g 封裝 → N3 NIC → gNB

上行：
N3 NIC → gtp5g 解封裝 → upfgtp / Linux routing → N6 / DN
```

---

## 5. Flow-Level 實驗設計

### 5.1 為什麼用 IP filter

目前只有一個 UE 與一個 `uesimtun0`，因此不能再靠不同 `uesimtun` 區分 flow。

flow-level 應用以下方式區分：

```text
同一個 UE
同一個 uesimtun0
同一個 PDU Session
同一個 TEID
不同目的 IP / port / filter
→ 不同 QoS Flow / QFI
```

由於 WebConsole 的 SDF filter 欄位目前只支援一個 CIDR 或 CIDR + port，因此最簡做法是用不同目的 IP 區分 flow。

### 5.2 DN namespace 多 IP 設定

目前 DN namespace 可掛多個 IP：

```bash
sudo ip netns add dn

sudo ip link add veth-upf type veth peer name veth-dn

sudo ip addr add 10.200.0.1/24 dev veth-upf
sudo ip link set veth-upf up

sudo ip link set veth-dn netns dn
sudo ip netns exec dn ip addr add 10.200.0.2/24 dev veth-dn
sudo ip netns exec dn ip addr add 10.200.0.3/24 dev veth-dn
sudo ip netns exec dn ip addr add 10.200.0.4/24 dev veth-dn
sudo ip netns exec dn ip link set veth-dn up
sudo ip netns exec dn ip link set lo up

sudo ip netns exec dn ip route add 10.60.0.0/16 via 10.200.0.1
```

角色設計：

```text
10.200.0.2/32 → Flow A
10.200.0.3/32 → Flow B
0.0.0.0/0     → Default Flow
```

環境清除:
```
sudo ip netns del dn 2>/dev/null || true
sudo ip link del veth-upf 2>/dev/null || true
```

---

## 6. WebConsole / MongoDB 設定

### 6.1 subscriptionData.provisionedData.smData

目前 UE / DNN / Slice 設定：

```javascript
db.getCollection("subscriptionData.provisionedData.smData").find().pretty()
```

觀察到：

```text
ueId = imsi-208930000000001
servingPlmnId = 20893
singleNssai = SST=1, SD=010203
DNN = internet
default 5QI = 9
sessionAmbr = 1000 Mbps / 1000 Mbps
```

此部分代表 default PDU Session 與 default QoS profile。

### 6.2 policyData.ues.flowRule

目前 flowRule 設定：

```javascript
db.getCollection("policyData.ues.flowRule").find().pretty()
```

結果：

```text
10.200.0.2/32 → qosRef=1 → precedence=64
10.200.0.3/32 → qosRef=2 → precedence=128
0.0.0.0/0     → qosRef=3 → precedence=255
```

意義：

```text
更精準的 IP filter 使用較小 precedence
default 0.0.0.0/0 使用最大 precedence
```

目前 precedence 設定已由原本錯誤順序修正為：

```text
10.200.0.2/32 precedence 64
10.200.0.3/32 precedence 128
0.0.0.0/0     precedence 255
```

### 6.3 policyData.ues.qosFlow

目前 qosFlow 設定：

```javascript
db.getCollection("policyData.ues.qosFlow").find().pretty()
```

結果：

```text
qosRef=1 → 5QI=7
qosRef=2 → 5QI=8
qosRef=3 → 5QI=9
```

搭配 flowRule 應理解為：

```text
10.200.0.2/32 → qosRef=1 → 5QI=7
10.200.0.3/32 → qosRef=2 → 5QI=8
0.0.0.0/0     → qosRef=3 → 5QI=9
```

---

## 7. Pcap / tcpdump 驗證方式

### 7.1 N3 GTP-U pcap：看實際 QFI

N3 是 gNB ↔ UPF 的 GTP-U user-plane，使用 UDP port 2152。

```bash
sudo tcpdump -i enp0s8 -nn -s 0 -w n3_qfi_test.pcap udp port 2152
```

Wireshark 展開：

```text
GPRS Tunneling Protocol
→ Extension Header: PDU Session Container
→ PDU Session Container
→ QoS Flow Identifier (QFI)
```

### 7.2 NGAP / NAS pcap：看 QFI / 5QI control-plane

NGAP 是 gNB ↔ AMF 的 N2 control-plane，使用 SCTP port 38412。

```bash
sudo tcpdump -i enp0s8 -nn -s 0 -w ngap_qos.pcap sctp port 38412
```

Wireshark 找：

```text
PDUSessionResourceSetupRequest
→ QoSFlowSetupRequestList
→ QFI
→ 5QI
```

目前觀察到 NGAP 中確實有多個 QoS Flow：

```text
QFI 1 → 5QI 9
QFI 4 → 5QI 8
QFI 2 → 5QI 9
QFI 3 → 5QI 7
```

代表 control-plane 已建立多個 QoS Flow。

### 7.3 NAS Authorized QoS Rules

NAS 被包在 NGAP 的 `pDUSessionNAS-PDU` 中：

```text
PDUSessionResourceSetupRequest
→ pDUSessionNAS-PDU
→ NAS-5GS
→ PDU Session Establishment Accept
→ Authorized QoS rules
→ Packet filter list
```

但目前觀察：

```text
pDUSessionNAS-PDU 顯示為 Encrypted data
```

因此無法直接從 Wireshark 查看 NAS QoS rule / packet filter。

### 7.4 PFCP / N4 pcap：看 SMF → UPF PDR / QER

PFCP 是 SMF ↔ UPF 的 N4 control-plane，使用 UDP port 8805。

```bash
sudo tcpdump -i any -nn -s 0 -w pfcp_n4.pcap udp port 8805
```

Wireshark 找：

```text
PFCP Session Establishment Request
PFCP Session Modification Request
→ Create PDR
→ Create QER
→ SDF Filter
→ QER ID
→ QFI
```

---

## 8. PFCP SDF Filter / Flow Description 觀察

在 PFCP Session Modification Request 的 PDR 中看到：

```text
Source Interface: Core
UE IP Address: Destination IP address = 10.60.0.1
SDF Filter:
  Flow Description: permit out ip from 10.200.0.3/32 to assigned
```

此 flow description 意義為：

```text
匹配 downlink：
10.200.0.3 → assigned UE IP
```

其中：

```text
assigned = UE 被分配到的 IP，例如 10.60.0.1
```

因此：

```text
permit out ip from 10.200.0.3/32 to assigned
= 10.200.0.3 → 10.60.0.1
= DN → UE
= downlink flow
```

`permit out` 在這裡不是 Linux egress，而是 SDF / 5G policy 的方向語意，通常可理解為 toward UE / downlink。

對應 uplink 的概念則應該類似：

```text
permit in ip from assigned to 10.200.0.3/32
```

但目前 WebConsole / PFCP 看到的是 downlink-oriented filter。

---

## 9. 目前最重要發現：data-plane 仍為 QFI=1

目前已確認：

```text
MongoDB flowRule：有
MongoDB qosFlow：有
NGAP QoSFlowSetupRequestList：有多個 QFI / 5QI
PFCP PDR：有 SDF filter
N3 uplink GTP-U：QFI=1
N3 downlink GTP-U：QFI=1
```

### 9.1 上行觀察

上行：

```text
10.60.0.1 → 10.200.0.2
10.60.0.1 → 10.200.0.3
```

N3 GTP-U 觀察：

```text
PDU Type: UL PDU SESSION INFORMATION
QFI=1
```

### 9.2 下行觀察

下行：

```text
10.200.0.3 → 10.60.0.1
```

N3 GTP-U 觀察：

```text
PDU Type: DL PDU SESSION INFORMATION
QFI=1
```

代表：

```text
即使 control-plane 與 PFCP 有多 QoS Flow / flowRule 資訊，
實際 user-plane GTP-U PDU Session Container 仍全部攜帶 QFI=1。
```

---

## 10. QER 觀察與目前推論

在 PFCP 中觀察到：

```text
每個 PDR 都會掛到 2 個 QER
其中都包含 QER ID=1
QER ID=1 對應 QFI=1
```

此情況可推論為：

```text
QER ID=1 = default / session-level QER
另一個 QER ID = flow-level / dedicated QER
```

但因實際 GTP-U 仍為 QFI=1，目前最合理判斷是：

```text
UPF / gtp5g 在某個 PDR 同時套用多個 QER 時，
實際封裝 GTP-U PDU Session Container 的 QFI 選到了 default QER=1，
因此最後上下行實際封包都呈現 QFI=1。
```

證據鏈：

```text
1. MongoDB flowRule 有設定
   10.200.0.2/32 → qosRef=1
   10.200.0.3/32 → qosRef=2
   0.0.0.0/0     → qosRef=3

2. MongoDB qosFlow 有設定
   qosRef=1 → 5QI=7
   qosRef=2 → 5QI=8
   qosRef=3 → 5QI=9

3. NGAP 有建立多個 QoS Flow
   QFI 1 → 5QI 9
   QFI 4 → 5QI 8
   QFI 2 → 5QI 9
   QFI 3 → 5QI 7

4. PFCP PDR 有 SDF filter
   permit out ip from 10.200.0.3/32 to assigned

5. 每個 PDR 掛多個 QER
   其中都包含 QER ID=1

6. QER ID=1 對應 QFI=1

7. 實際 N3 GTP-U uplink / downlink 仍 QFI=1
```

因此可寫成：

> 實驗結果顯示，雖然 WebConsole、PCF/SMF policy、NGAP QoSFlowSetupRequestList 與 PFCP PDR/QER 均建立了多個 QoS Flow 相關資訊，但實際 GTP-U user-plane 封包仍全部攜帶 QFI=1。進一步觀察發現，每個 PDR 皆同時關聯 QER ID=1，而 QER ID=1 對應 default QFI=1。此現象表示目前 free5GC UPF / gtp5g 在多 QER 情境下，實際寫入 GTP-U PDU Session Container 的 QFI 可能仍以 default QER 為主，導致 dedicated QoS Flow 未真正落地至 data-plane QFI differentiation。

---

## 11. 對研究的意義

這個結果對論文非常有價值，因為它支持原本研究假設：

```text
free5GC control-plane 可建立 QoS Flow / QFI / 5QI
但 user-plane / gtp5g 未必真正將 QoS Flow 落地到 data-plane differentiation
```

目前 baseline finding 可以寫成：

> free5GC 可以在 control-plane 中建立多個 QoS Flow，NGAP 也能觀察到不同 QFI 與 5QI 的 mapping；PFCP 中亦可觀察到 flowRule 對應的 PDR / QER。然而，在實際 GTP-U user-plane 封包中，上下行仍皆攜帶 default QFI=1。此結果顯示 QoS Flow 的 control-plane 設定與 data-plane 實際 QFI differentiation 之間存在落差。

---

## 12. 下一步建議

### 12.1 程式碼追蹤

接下來應進入 UPF / gtp5g 程式碼確認：

```text
downlink 封裝時從哪個 QER 取 QFI
多個 QER 同時存在時選擇邏輯為何
是否固定取 default QER=1
是否忽略 dedicated QER 的 QFI
```

建議搜尋關鍵字：

```bash
grep -R "QER" -n .
grep -R "qfi" -ni .
grep -R "QFI" -n .
grep -R "PDR" -n .
grep -R "FAR" -n .
grep -R "pdr.qer" -ni .
grep -R "qer_id" -ni .
grep -R "PDU Session Container" -ni .
grep -R "gtpu" -ni .
```

重點檢查：

```text
PDR match 後取得 QER list
多 QER 如何選 QFI
GTP-U PDU Session Container 的 QFI 寫入位置
```

### 12.2 實驗方向選擇

有兩條方向：

#### 方向 A：修正 / 修改 gtp5g QFI 選擇邏輯

目標：

```text
PDR 命中 10.200.0.3/32
→ 選 dedicated QER
→ 寫入 dedicated QER 的 QFI
→ N3 downlink 出現非 QFI=1
```

#### 方向 B：維持 baseline finding，轉為 Method A metadata injection

即使 free5GC 原生 QFI 未落地，也可在 gtp5g 修改：

```text
根據 TEID / PDR / inner IP / target flow
→ 自行設定 skb->mark / skb->priority
→ Linux TC classification
```

但此時論文要清楚說明：

```text
原生 free5GC / gtp5g 未能將 dedicated QoS Flow 落地到 data-plane QFI differentiation
本研究 Method A 是補上這段落地機制
```

---

## 13. 目前可放進論文的核心結論草稿

```text
The experiment shows that free5GC is able to create multiple QoS Flow contexts at the control-plane level. The NGAP PDU Session Resource Setup Request contains multiple QoS Flow Setup Request Items with different QFIs and 5QIs, and PFCP messages also include flow-specific PDR and QER information derived from WebConsole policy data. However, packet-level observation on the N3 interface shows that both uplink and downlink GTP-U packets still carry QFI=1 in the PDU Session Container, regardless of the matched destination IP flow. Further PFCP inspection indicates that each PDR is associated with multiple QERs, including QER ID=1, which corresponds to the default QFI=1. This suggests that, in the observed free5GC / gtp5g behavior, the data-plane encapsulation still selects or falls back to the default QER when writing the QFI into GTP-U packets, and the dedicated QoS Flow is not effectively reflected in user-plane QFI differentiation.
```

中文版本：

```text
實驗結果顯示，free5GC 能在控制面建立多個 QoS Flow。NGAP 的 PDU Session Resource Setup Request 中可觀察到多個 QFI 與 5QI 的對應關係，PFCP 訊息中也可看到由 WebConsole policy 產生的 flow-specific PDR 與 QER。然而，在 N3 介面實際擷取的 GTP-U 封包中，無論上行或下行、無論目的 IP 是否命中不同 flowRule，PDU Session Container 中的 QFI 皆仍為 1。進一步觀察 PFCP 發現，每個 PDR 皆同時關聯多個 QER，且都包含 QER ID=1，而 QER ID=1 對應 default QFI=1。此現象表示目前 free5GC / gtp5g 在實際 user-plane 封裝時，可能仍選用或回落至 default QER，導致 dedicated QoS Flow 未真正反映為 data-plane 的 QFI 差異化。
```

---

## 14. 簡短總結

目前狀態可用一句話整理：

> WebConsole / MongoDB / NGAP / PFCP 都已經看得到 flow-level QoS 設定，但實際 N3 GTP-U 上下行封包仍全部是 QFI=1；目前最合理推論是 UPF / gtp5g 在多 QER 情境下實際封裝時選到了 default QER=1，導致 dedicated QoS Flow 沒有真正落地到 user-plane QFI differentiation。

# 最終修正方式與驗證結果補充

更新日期：2026-06-23  
主題：free5GC / gtp5g Downlink Flow-Level QFI Differentiation 修正與驗證

---

## 1. 問題背景

原本 flow-level 實驗目標是：

```text
同一個 UE
同一個 PDU Session
同一個 downlink TEID
不同 DN flow
→ 對應不同 QoS Flow / QFI
```

實驗設計為：

```text
10.200.0.2 → 10.60.0.1
10.200.0.3 → 10.60.0.1
其他 downlink traffic → 10.60.0.1
```

修正前，即使 WebConsole / MongoDB 中已有多條 `flowRule` 與 `qosFlow`，N3 GTP-U 實際觀察仍全部為：

```text
PDU Session Container QFI = 1
```

這表示 control-plane 雖然有 QoS Flow 設定，但 data-plane 尚未正確反映 flow-specific QFI。

---

## 2. 原本問題與修正方向

修正前觀察到：

```text
每個 PDR 都會掛多個 QER
其中都包含 QER ID=1
QER ID=1 → QFI=1
```

後續釐清後，真正問題不是「PDR 不該掛 QER1」，而是：

```text
QER1 是 session-level AMBR QER
它應該保留在每個 PDR 上，用於整個 PDU Session 的總速率控制

但 GTP-U PDU Session Container 的 QFI
不應取自 QER1
而應取自 flow-specific QER
```

因此正確設計為：

```text
PDR 命中特定 flow
→ 套用 QER1 做 session-level AMBR
→ 套用 flow-specific QER 決定 QFI
→ GTP-U PDU Session Container 寫入 flow-specific QER 的 QFI
```

---

## 3. QER 的正確角色

| QER | QFI | 用途 |
|---|---:|---|
| QER ID 1 | QFI=1 | Session AMBR / session-level enforcement |
| QER ID 2 | QFI=1 | Default QoS Flow |
| QER ID 3 | QFI=2 | Flow-specific QER |
| QER ID 4 | QFI=3 | Flow-specific QER |
| QER ID 5 | QFI=4 | Flow-specific QER |

重點：

```text
QER1 是 session-level QER，不是 flow-specific QER。
QER2 是 default QoS Flow QER。
QER3 / QER4 / QER5 是根據 WebConsole flowRule / qosFlow 產生的 dedicated QER。
```

---

## 4. qosRef 與 QFI 的差異

這次修正中也釐清：

```text
qosRef 不等於 QFI
```

`qosRef` 是 WebConsole / policy data 中用來關聯：

```text
flowRule → qosFlow
```

QFI 則是 SMF 實際分配給 QoS Flow 的 data-plane identifier。  
目前程式中由 SMF 呼叫 `AssignQFI()` 進行動態分配，且 dedicated QoS Flow 的 QFI 從 2 開始分配。

因此不能假設：

```text
qosRef=1 → QFI=1
qosRef=2 → QFI=2
qosRef=3 → QFI=3
```

此外，flowRule 會先被放入 Go map，後續透過 map iteration 處理，因此 flowRule 的處理順序不保證等於 WebConsole 顯示順序，也不保證等於 qosRef 1、2、3 的排序。

---

## 5. 最終 PFCP 結構

最新 PFCP pcap 顯示，SMF 已產生 5 個 QER 與 8 個 PDR。

### 5.1 QER 統整

| QER ID | QFI | 角色 |
|---|---:|---|
| QER CP 1 | 1 | Session AMBR / session-level enforcement |
| QER CP 2 | 1 | Default QoS Flow |
| QER CP 3 | 2 | Flow-specific QER |
| QER CP 4 | 3 | Flow-specific QER |
| QER CP 5 | 4 | Flow-specific QER |

### 5.2 PDR 統整

| PDR | Source Interface | Flow Description | Precedence | QER |
|---:|---|---|---:|---|
| PDR 1 | Access | `permit out ip from any to assigned` | 255 | QER 2 + QER 1 |
| PDR 2 | Core | `permit out ip from any to assigned` | 255 | QER 2 + QER 1 |
| PDR 3 | Access | `permit out ip from 10.200.0.2/32 to assigned` | 64 | QER 3 + QER 1 |
| PDR 4 | Core | `permit out ip from 10.200.0.2/32 to assigned` | 64 | QER 3 + QER 1 |
| PDR 5 | Access | `permit out ip from 10.200.0.3/32 to assigned` | 128 | QER 4 + QER 1 |
| PDR 6 | Core | `permit out ip from 10.200.0.3/32 to assigned` | 128 | QER 4 + QER 1 |
| PDR 7 | Access | `permit out ip from 0.0.0.0/0 to assigned` | 255 | QER 5 + QER 1 |
| PDR 8 | Core | `permit out ip from 0.0.0.0/0 to assigned` | 255 | QER 5 + QER 1 |

---

## 6. Downlink 應看 Core PDR

因為 downlink 路徑是：

```text
DN / Core
→ UPF
→ gNB
→ UE
```

所以 downlink flow 應主要對應 `Source Interface = Core` 的 PDR：

| Downlink flow | Core PDR | Dedicated QER | 預期 QFI |
|---|---:|---:|---:|
| `10.200.0.2 → 10.60.0.1` | PDR 4 | QER 3 | QFI=2 |
| `10.200.0.3 → 10.60.0.1` | PDR 6 | QER 4 | QFI=3 |
| `8.8.8.8 → 10.60.0.1` / other traffic | PDR 8 | QER 5 | QFI=4 |
| generic default | PDR 2 | QER 2 | QFI=1 |

---

## 7. 最終 N3 Downlink 驗證結果

最新 N3 downlink pcap 顯示：

| Inner flow | 實際 QFI | PFCP 預期 |
|---|---:|---|
| `10.200.0.2 → 10.60.0.1` | QFI=2 | PDR 4 → QER 3 → QFI=2 |
| `10.200.0.3 → 10.60.0.1` | QFI=3 | PDR 6 → QER 4 → QFI=3 |
| `8.8.8.8 → 10.60.0.1` | QFI=4 | PDR 8 → QER 5 → QFI=4 |

Wireshark 中可觀察到：

```text
GPRS Tunneling Protocol
→ Extension Header: PDU Session Container
→ PDU Type: DL PDU SESSION INFORMATION
→ QoS Flow Identifier (QFI)
```

代表：

```text
Inner IP: 10.200.0.2 → 10.60.0.1
QFI=2

Inner IP: 10.200.0.3 → 10.60.0.1
QFI=3

Inner IP: 8.8.8.8 → 10.60.0.1
QFI=4
```

---

## 8. 是否證明修正正確？

可以。以目前 PFCP 與 N3 pcap 交叉比對，已經可以支持：

```text
UPF / gtp5g 有依據 downlink PDR 命中結果，
使用對應 flow-specific QER 的 QFI，
而不是錯選 QER1 的 QFI=1。
```

嚴格來說，pcap 不能直接看到程式內部選了哪個 QER 物件，但因為：

```text
PFCP PDR/QER/QFI 對應
與
N3 GTP-U 實際 QFI
完全一致
```

所以這是很強的外部證據，可用來證明修正後的行為正確。

---

## 9. 修正後的重要結論

> 修改後，SMF 能正確產生 flow-specific PDR/QER，並保留 QER1 作為 session-level AMBR enforcement。N3 downlink pcap 顯示，UPF / gtp5g 在封裝 downlink GTP-U 時，並未錯選 QER1 的 default QFI=1，而是依據命中的 flow-specific PDR 選用對應 QER 的 QFI。`10.200.0.2 → 10.60.0.1` 被封裝為 QFI=2，`10.200.0.3 → 10.60.0.1` 被封裝為 QFI=3，其他符合 `0.0.0.0/0` 的 flow 則被封裝為 QFI=4。此結果證明 downlink QoS Flow-level differentiation 已成功落地到 GTP-U user-plane。

---

## 10. 研究意義

這個結果證明以下證據鏈已經打通：

```text
WebConsole / MongoDB policy
→ PCF / SMF policy handling
→ PFCP PDR / QER
→ UPF / gtp5g downlink GTP-U encapsulation
→ N3 PDU Session Container QFI
```

可以將研究結論從原本的 baseline finding：

```text
control-plane 有多 QoS Flow，但 data-plane 仍 QFI=1
```

更新為：

```text
經修改 SMF 後，flow-specific PDR/QER 能正確下發至 UPF，
且 downlink GTP-U 封包能依據不同 flow 寫入不同 QFI。
```

因此目前可主張：

```text
Downlink QoS Flow-level QFI differentiation 已完成
```

但仍需注意：

```text
目前成功的是 downlink flow-level QFI differentiation
尚不等於完整 slice-level resource isolation
也尚未完成 uplink flow-level QFI differentiation
```

---

## 11. 後續可接 Method A

接下來可將 downlink QFI 接到 Linux TC / skb metadata：

```text
QFI=2 → skb->mark / skb->priority → TC class A
QFI=3 → skb->mark / skb->priority → TC class B
QFI=4 → skb->mark / skb->priority → TC class C
```

也就是：

```text
5G QoS Flow identity
→ GTP-U QFI
→ gtp5g skb metadata
→ Linux TC classification
→ priority scheduling / rate control
```

---

## 12. 最終摘要

```text
本次修正後，SMF 已能同時保留 session-level QER1 與 flow-specific QER。PFCP 中，10.200.0.2/32、10.200.0.3/32 與 0.0.0.0/0 分別對應不同 Core PDR 與 dedicated QER，並分別配置 QFI=2、QFI=3、QFI=4。N3 downlink pcap 進一步證明，實際 GTP-U PDU Session Container 中的 QFI 與 PFCP 預期一致。這表示 UPF / gtp5g 在 downlink 封裝時已能選用正確的 flow-specific QER，而不是回落到 session-level QER1 的 QFI=1。此結果證明 downlink QoS Flow-level differentiation 已成功落地到 user-plane。
```
