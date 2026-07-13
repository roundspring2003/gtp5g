# free5GC Flow Rules 到 NGAP QoS Flow 與 PFCP PDR/QER 流程

## 目的

本文整理 free5GC 中 webconsole Flow Rules 如何被寫入 MongoDB，如何由 PCF 解析為 `SmPolicyDecision` 的 `PccRules` / `QosDecs`，再由 SMF 轉成：

- 給 gNB 的 NGAP `QosFlowSetupRequestList` / `QosFlowAddOrModifyRequestList`
- 給 UPF 的 PFCP `PDR` / `QER`
- 給 UE 的 NAS QoS Rules / Authorized QoS Flow Descriptions

目前實驗已確認 webconsole 設立三個 Flow Rules，MongoDB 已有：

| IP Filter | qosRef | 5QI |
| --- | ---: | ---: |
| `10.200.0.2/32` | 1 | 7 |
| `10.200.0.3/32` | 2 | 8 |
| `0.0.0.0/0` | 3 | 9 |

核心判斷：`qosRef` 不是 QFI。`qosRef` 只是 webconsole / MongoDB / PCF 中連接 `FlowRules` 與 `QosFlows` 的資料參照；真正的 QFI 是 SMF 收到 PCF policy 後，透過 `AssignQFI()` 配出來。free5GC 預設保留 QFI `1` 給 default QoS，所以 flow-level rules 若成功生效，通常會產生額外的 QFI，例如 2、3、4。

本次後續修正已讓 SMF 在處理 flow-level PCC rules 時依 `RefQosData` / `qosRef` 的數字順序執行，因此在目前 webconsole 設定下，QFI 分配可固定為：

| qosRef | IP Filter | 預期 QFI |
| ---: | --- | ---: |
| 1 | `10.200.0.2/32` | 2 |
| 2 | `10.200.0.3/32` | 3 |
| 3 | `0.0.0.0/0` | 4 |

## 整體流程

```text
webconsole 表單
  -> FlowRules / QosFlows / ChargingDatas
  -> MongoDB policyData.ues.*
  -> PCF Create SM Policy
  -> SmPolicyDecision: PccRules + QosDecs
  -> SMF ApplyPccRules()
  -> DataPath + QoSFlow + QER + PDR
  -> PFCP Session Establishment/Modification to UPF
  -> N1N2MessageTransfer to AMF
  -> NGAP PDUSessionResourceSetupRequest to gNB
```

## 1. webconsole 如何建立 Flow Rules

webconsole 前端的 Flow Rule 表單欄位在 `webconsole/frontend/src/pages/SubscriberCreate/FormFlowRule.tsx`：

- `filter`：畫面標籤為 `IP Filter`
- `precedence`
- `5qi`
- `gbrUL` / `gbrDL`
- `mbrUL` / `mbrDL`
- flow-level charging data

表單資料送出前，`webconsole/frontend/src/lib/dtos/subscription.ts` 會把巢狀表單轉成後端 API 使用的平面資料。關鍵邏輯在 `FlowsMapperImpl.map()`：

1. 每個 DNN 下的 flow rule 依序配置 `qosRef`，從 `1` 開始遞增。
2. 寫入 `FlowRules[]`：
   - `filter`
   - `precedence`
   - `snssai`
   - `dnn`
   - `qosRef`
3. 寫入 `QosFlows[]`：
   - `snssai`
   - `dnn`
   - `qosRef`
   - `5qi`
   - `mbrUL` / `mbrDL`
   - `gbrUL` / `gbrDL`
4. 寫入 `ChargingDatas[]`，flow-level charging data 也會帶同一個 `qosRef`。

因此同一條 Flow Rule 的關聯方式是：

```text
FlowRules.qosRef == QosFlows.qosRef == ChargingDatas.qosRef
```

後端資料模型在：

- `webconsole/backend/WebUI/model_flow_rule.go`
- `webconsole/backend/WebUI/model_qos_flow.go`

其中 `FlowRule` 有 `Filter`、`Precedence`、`Snssai`、`Dnn`、`QosRef`；`QosFlow` 有 `Snssai`、`Dnn`、`QosRef`、`5qi`、MBR、GBR。

## 2. webconsole 如何寫入 MongoDB

webconsole 後端的 collection 名稱在 `webconsole/backend/WebUI/api_webui.go`：

| 資料 | Mongo collection |
| --- | --- |
| Flow Rules | `policyData.ues.flowRule` |
| QoS Flows | `policyData.ues.qosFlow` |
| Charging Data | `policyData.ues.chargingData` |
| SM Policy Data | `policyData.ues.smData` |

建立或更新 subscriber 時，`dbOperation()` 會：

1. 把 `FlowRules[]` 加上 `ueId`、`servingPlmnId` 後寫入 `policyData.ues.flowRule`。
2. 把 `QosFlows[]` 加上 `ueId`、`servingPlmnId` 後寫入 `policyData.ues.qosFlow`。
3. 把 `ChargingDatas[]` 加上查詢鍵後寫入 `policyData.ues.chargingData`。

讀取 subscriber 時，後端也會從這三個 collection 組回 `SubsData.FlowRules`、`SubsData.QosFlows`、`SubsData.ChargingDatas`。

## 3. SMF 如何向 PCF 建立 SM Policy Association

PDU Session 建立時，SMF 在 `NFs/smf/internal/sbi/processor/pdu_session.go` 呼叫：

```text
SendSMPolicyAssociationCreate(smContext)
```

SMF 送給 PCF 的 `SmPolicyContextData` 由 `NFs/smf/internal/sbi/consumer/pcf_service.go` 組成，重要欄位包括：

- `Supi`
- `PduSessionId`
- `Dnn`
- `SliceInfo`
- `Ipv4Address`
- `SubsSessAmbr`
- `SubsDefQos`
- `ServingNetwork`

PCF 回傳 `SmPolicyDecision` 後，SMF 先套用 session rule，再套用 PCC rules：

```text
ApplySessionRules(smPolicyDecision)
ApplyPccRules(smPolicyDecision)
```

## 4. PCF 如何解析 MongoDB 的 Flow Rules

PCF 入口是 `NFs/pcf/internal/sbi/processor/smpolicy.go` 的 `HandleCreateSmPolicyRequest()`。

PCF 建立 SM policy decision 時會做兩件事：

1. 建立 default session rule 與 default PCC rule。
2. 從 MongoDB 讀取 `qosFlow` 與 `flowRule`，轉成 `QosDecs` 與 flow-level `PccRules`。

PCF 查詢 flow rule / qos flow 使用的 filter 是：

```text
ueId   = request.Supi
snssai = util.SnssaiModelsToHex(*request.SliceInfo)
dnn    = request.Dnn
```

注意：PCF 這裡沒有把 `servingPlmnId` 放入查詢條件。如果資料庫中同一個 UE 有跨 PLMN 或殘留資料，可能會影響匹配結果。

### 4.1 QosFlows 轉成 QosDecs

PCF 先讀 `policyData.ues.qosFlow`，每筆資料經 `newQosDataWithQosFlowMap()` 轉成 `models.QosData`：

```text
QosData.QosId  = strconv.Itoa(qosRef)
QosData.Var5qi = 5qi
QosData.MaxbrUl = mbrUL
QosData.MaxbrDl = mbrDL
QosData.GbrUl = gbrUL
QosData.GbrDl = gbrDL
```

因此你的資料預期會先變成：

| QosDecs key / QosId | 5QI |
| --- | ---: |
| `"1"` | 7 |
| `"2"` | 8 |
| `"3"` | 9 |

### 4.2 FlowRules 轉成 PccRules

PCF 再讀 `policyData.ues.flowRule`。每筆 flow rule 會：

1. 讀出 `precedence`。
2. 讀出 `filter` 字串。
3. 以 `strings.Split(filter, " ")` 拆開。
4. 建立 `flowdesc.NewIPFilterRule()`：
   - `Action = permit`
   - `Dir = out`
   - `Src = tokens[0]`
   - `Dst = assigned`
5. 若 `tokens[1]` 存在，PCF 會嘗試把它解析成 `portLower-portUpper`。
6. 用 `flowdesc.Encode()` 轉成 PCC rule 的 `FlowDescription`。
7. 建立 `models.PccRule`，其中 `FlowDirection = DOWNLINK`。
8. 用 `qosRef` 把此 PCC rule 關聯到 `QosDecs`。

這代表目前這版 webconsole 的 `IP Filter` 欄位不是完整 5-tuple flow description，而是 PCF 期待的簡化格式：

```text
<source-cidr>
<source-cidr> <port-lower>-<port-upper>
```

例如：

```text
10.200.0.2/32
10.200.0.2/32 8000-8000
```

若填入完整字串如 `permit out ip from 10.200.0.2/32 to assigned`，PCF 會把第一個 token `permit` 當成 `Src`，導致產生錯誤或無效的 SDF filter。

### 4.3 qosRef 關聯的關鍵條件

PCF 使用 `SetPccRuleRelatedByQosRef()` 把 PCC rule 關到 QoS data。這個函式有一個非常重要的條件：

```text
如果 decision.QosDecs == nil 或 decision.QosDecs[qosRef] == nil，直接 return
```

也就是說，只要 `QosFlows` 沒被 PCF 查到、`qosRef` 對不上、`snssai/dnn/ueId` 對不上，該 Flow Rule 的 PCC rule 就不會放進 `decision.PccRules`，後面 SMF 也就不會建立 flow-level QFI/QER/PDR。

## 5. SMF 如何把 PccRules 轉成 QFI、NGAP QoS Flow、PFCP QER/PDR

SMF 的核心在 `NFs/smf/internal/context/sm_context_policy.go` 的 `ApplyPccRules()`。

### 5.1 default QoS 永遠是 QFI 1

`NFs/smf/internal/context/session_rules.go` 的 `NewSessionRule()` 固定：

```text
DefQosQFI = 1
```

而 `NFs/smf/internal/context/sm_context.go` 初始化 QFI generator 時：

```text
QFIGenerator = NewGenerator(2, 63)
```

註解也寫明：`1 always reserve for default Qos`。

所以：

- QFI `1` 是 default QoS。
- Flow-level QoS 若成功建立，會從 QFI `2` 開始配置。

### 5.2 flow-level PCC rule 如何取得 QFI

`ApplyPccRules()` 會處理 PCF 回來的每個 `PccRule`。若該 rule 有 `RefQosData`：

```text
tgtQosID := tgtPcc.RefQosDataID()
tgtPcc.SetQFI(c.AssignQFI(tgtQosID))
```

`AssignQFI(qosId)` 會檢查 `qosDataToQFI`：

- 若該 `qosId` 已經配過 QFI，沿用既有 QFI。
- 若沒有，從 `QFIGenerator` 配新的 QFI。

原本 `decision.PccRules` 與 SMF 內部待處理 rules 都是 Go map，若直接 `range` map 呼叫 `AssignQFI()`，三個 `qosRef` 的第一次出現順序不保證固定。因此早期只能保證會出現 QFI 2、3、4，不能保證哪個 flow 對應哪個 QFI。

後續修正已在 `NFs/smf/internal/context/sm_context_policy.go` 加入 `sortedPCCRuleIDsByQoSRef()`，在 `ApplyPccRules()` 的 PDU-session pass 與 flow-level pass 都依 `RefQosDataID()` 排序後再呼叫 `processRule()` / `AssignQFI()`。排序規則為：

1. 若 `RefQosDataID()` 是純數字，依數字小到大排序。
2. 若不是純數字，fallback 依 `RefQosDataID()` 字串排序。
3. 若 QoS ID 相同，再用 `PccRuleId` 排序，避免同值時仍受 map iteration 影響。

因此目前三個 `qosRef` 若都成功進入 SMF，預期固定為：

```text
qosRef=1 -> QFI=2
qosRef=2 -> QFI=3
qosRef=3 -> QFI=4
```

此修正不改 `AssignQFI()` 本身；`AssignQFI()` 仍維持「第一次看到 qosId 就從 QFI generator 配下一個 QFI」。修正點是讓「第一次看到」的順序變成可預期。

### 5.3 建立 DataPath、PDR、QER

SMF 針對每個 PCC rule 呼叫 `CreatePccRuleDataPath()`：

1. 選出 UPF path。
2. `ActivateTunnelAndPDR()` 建立 UL/DL tunnel 與 PDR。
3. `applyFlowInfoOrPFD()` 呼叫 `PCCRule.UpdateDataPathFlowDescription()`。
4. `DataPath.UpdateFlowDescription()` 把 flow description 寫進 UL/DL PDR 的 `PDI.SDFFilter`。
5. 若 PCC rule 有 `RefQosData`，呼叫：

```text
pccRule.Datapath.AddQoS(c, pccRule.QFI, qosData)
c.AddQosFlow(pccRule.QFI, qosData)
```

其中：

- `AddQoS()` 會建立或取得 UPF 上對應 QFI 的 QER。
- `AddQosFlow()` 會把此 QoS flow 放入 `SMContext.AdditonalQosFlows`，供 NGAP / NAS 使用。

## 6. PFCP PDR / QER 如何下到 UPF

PFCP 相關資料結構在 `NFs/smf/internal/context/pfcp_rules.go`：

- `PDR`
  - `PDRID`
  - `Precedence`
  - `PDI`
  - `FAR`
  - `URR`
  - `QER`
- `PDI`
  - `SourceInterface`
  - `LocalFTeid`
  - `NetworkInstance`
  - `UEIPAddress`
  - `SDFFilter`
- `QER`
  - `QERID`
  - `QFI`
  - `GateStatus`
  - `MBR`
  - `GBR`

`DataPath.ActivateTunnelAndPDR()` 會先替 default path 建立 QFI 1 的 QER。flow-level rule 則由 `DataPath.AddQoS()` 建立額外 QER：

```text
QER.QFI = flow-level QFI
QER.GateStatus = open/open
QER.MBR = mbrUL/mbrDL
QER.GBR = gbrUL/gbrDL, 僅 GBR flow
```

接著 `AddQoS()` 會把這個 QER 掛到 UL/DL PDR：

```text
node.UpLinkTunnel.PDR.QER append(qer)
node.DownLinkTunnel.PDR.QER append(qer)
```

真正送 PFCP 時，`NFs/smf/internal/sbi/processor/datapath.go` 的 `ActivateUPFSession()` 會收集每個 UPF 的 PDR/FAR/QER/URR，然後呼叫 PFCP message builder。

`NFs/smf/internal/pfcp/message/build.go` 做最後轉換：

- `pdrToCreatePDR()`
  - 寫入 `PDRID`
  - 寫入 `Precedence`
  - 寫入 `PDI`
  - 若有 `PDI.SDFFilter`，放進 PFCP CreatePDR
  - 把 PDR 上的每個 QER 轉成 `QERID`
- `qerToCreateQER()`
  - 寫入 `QERID`
  - 寫入 `GateStatus`
  - 寫入 `QoSFlowIdentifier = QFI`
  - 寫入 `MaximumBitrate = MBR`
  - 寫入 `GuaranteedBitrate = GBR`

所以在 PFCP pcap 中，若 flow-level rules 生效，應看到：

- `CreateQER` 或 `UpdateQER` 中有非 1 的 `QoS Flow Identifier`
- `CreatePDR` 或 `UpdatePDR` 中有 `SDF Filter`
- 該 PDR 參照對應的 `QER ID`

## 7. NGAP QoS Flow 如何下到 gNB

NGAP QoS Flow 由 SMF 先組成 N2 SM information，再透過 AMF 轉送給 gNB。

SMF 在 PDU Session Establishment Accept 流程中建立：

```text
BuildPDUSessionResourceSetupRequestTransfer(smContext)
```

位置在 `NFs/smf/internal/context/ngap_build.go`。

這個 transfer 的 `QosFlowSetupRequestList` 一定先放 default QFI：

```text
QFI = sessRule.DefQosQFI = 1
5QI = AuthDefQos.Var5qi
```

接著它會遍歷：

```text
ctx.AdditonalQosFlows
```

並把每個 flow-level QoS flow 轉成 NGAP `QosFlowSetupRequestItem`：

```text
QosFlowIdentifier = flow-level QFI
NonDynamic5QI.FiveQI = QosData.Var5qi
GBRQosInformation = MBR/GBR, 若是 GBR flow
ARP = QosData.Arp 或預設 priority 8
```

SMF 將這份 N2 SM information 放到 `N1N2MessageTransfer`：

```text
NgapIeType = PDU_RES_SETUP_REQ
BinaryDataN2Information = PDUSessionResourceSetupRequestTransfer
```

AMF 在 `NFs/amf/internal/sbi/processor/n1n2message.go` 收到後，依 UE 是否已經 initial context setup：

- 已 setup：包成 `PDUSessionResourceSetupRequest`
- 尚未 setup：包成 `InitialContextSetupRequest` 內的 PDU session resource setup list

最後 `NFs/amf/internal/ngap/message/send.go` / `build.go` 把 SMF 產生的 transfer 原封不動放進 NGAP message 給 gNB。

## 8. NAS QoS Rules 給 UE 的部分

除了 NGAP 給 gNB、PFCP 給 UPF，SMF 也會在 `NFs/smf/internal/context/gsm_build.go` 建立 NAS `PDUSessionEstablishmentAccept`：

- default QoS rule：QFI 1，match all
- flow-level QoS rules：從 `smContext.PCCRules` 建出 packet filter 與對應 QFI
- authorized QoS flow descriptions：default QFI 1 加上 `AdditonalQosFlows`

這代表完整分流需要三邊一致：

1. UE 收到 NAS QoS rule，知道 uplink 封包該標哪個 QFI。
2. gNB 收到 NGAP QoS Flow，知道該 QFI 的 radio QoS 參數。
3. UPF 收到 PFCP PDR/QER，能依 SDF filter 與 QER 做 user-plane 分類與 QoS enforcement。

## 9. 以目前三條規則推導的預期結果

若 MongoDB 中三筆 `FlowRules` 與三筆 `QosFlows` 都被 PCF 查到，且 `qosRef` 對應正確，流程應為：

| Mongo | PCF | SMF |
| --- | --- | --- |
| `10.200.0.2/32 -> qosRef=1 -> 5QI=7` | `QosDecs["1"].Var5qi=7`，PCC rule `RefQosData=["1"]` | 固定配 QFI 2，建立 NGAP QoS Flow 與 PFCP QER |
| `10.200.0.3/32 -> qosRef=2 -> 5QI=8` | `QosDecs["2"].Var5qi=8`，PCC rule `RefQosData=["2"]` | 固定配 QFI 3，建立 NGAP QoS Flow 與 PFCP QER |
| `0.0.0.0/0 -> qosRef=3 -> 5QI=9` | `QosDecs["3"].Var5qi=9`，PCC rule `RefQosData=["3"]` | 固定配 QFI 4，建立 NGAP QoS Flow 與 PFCP QER |

NGAP 中預期看到：

- default QoS Flow：QFI 1，5QI 來自 DNN default QoS profile
- 額外 QoS Flows：三個非 1 QFI，5QI 分別為 7、8、9

PFCP 中預期看到：

- default QER：QFI 1
- flow-level QERs：QFI 2、3、4
- flow-level PDRs：帶 `SDF Filter`，並參照各自的 flow-level QERID

## 10. 為什麼可能仍然全部走 QFI 1

若目前觀察到不管 IP filter 如何都只走 QFI 1，通常代表 flow-level rule 沒有完整進入 UE/gNB/UPF 三邊的分類路徑。建議優先檢查以下分界點。

### 10.1 PCF 沒把 Flow Rule 放進 PccRules

最常見原因是 `SetPccRuleRelatedByQosRef()` 找不到對應的 `QosDecs[qosRef]`，因此直接 return。

檢查點：

- PCF 回給 SMF 的 `SmPolicyDecision.QosDecs` 是否有 `"1"`、`"2"`、`"3"`。
- PCF 回給 SMF 的 `SmPolicyDecision.PccRules` 是否有三條 flow-level PCC rules。
- 每條 PCC rule 是否有 `RefQosData=["1"]`、`["2"]`、`["3"]`。

若只有 default PCC rule，SMF 後面只會建立 QFI 1。

### 10.2 `ueId`、`snssai`、`dnn` 不匹配

PCF 讀 Mongo 時使用：

```text
ueId + snssai + dnn
```

任一欄位不一致都會讓 PCF 查不到 flow/qos 資料。尤其要確認：

- `snssai` 是否是 hex 格式，例如 `01010203`。
- `dnn` 是否與 PDU Session 使用的 DNN 完全一致。
- `ueId` 是否是 `imsi-...`，且與 `request.Supi` 一致。

### 10.3 `IP Filter` 欄位格式不符合 PCF 期待

PCF 目前只取 `strings.Split(filter, " ")[0]` 當 `Src`。因此建議先用最簡單格式測試：

```text
10.200.0.2/32
10.200.0.3/32
0.0.0.0/0
```

若要加 port，格式應類似：

```text
10.200.0.2/32 8000-8000
```

不要填完整的 `permit out ...` 字串，否則 PCF 會把 `permit` 當成來源 IP。

### 10.4 SMF 有 PccRules，但沒有呼叫 AddQoS

若 SMF 收到 PCC rule 但 `RefQosDataID()` 是空字串，就不會呼叫：

```text
pccRule.Datapath.AddQoS(...)
c.AddQosFlow(...)
```

結果是：

- PFCP 不會有 flow-level QER。
- NGAP `AdditonalQosFlows` 不會有非 1 QFI。

### 10.5 NGAP 有非 1 QFI，但 UE 仍標 QFI 1

這代表 gNB/UPF 可能已經拿到 QoS Flow，但 UE uplink NAS QoS rule 或 packet filter 沒有命中。要檢查 NAS `PDUSessionEstablishmentAccept` 中：

- Authorized QoS Rules 是否有 flow-level rule。
- Packet filter 是否符合實際 uplink 封包。
- 對應 QoS rule 的 QFI 是否為非 1。

### 10.6 PFCP 有非 1 QER，但 UPF PDR 沒有命中

若 PFCP 已有 QER，但資料仍走 default QER/QFI 1，檢查：

- flow-level PDR 是否有 `SDF Filter`。
- flow-level PDR 的 precedence 是否比 default PDR 更優先。
- SDF filter 方向與封包五元組是否吻合。
- 封包是 uplink 還是 downlink；PCF 目前建出的 flow description 是 `permit out ... from <filter> to assigned`，語意上較偏向 downlink server-to-UE 分類。

### 10.7 PDR 已命中 dedicated rule，但 GTP-U 仍全部是 QFI 1

本次實驗額外確認到一個更細的落點：即使 WebConsole、PCF/SMF policy、NGAP `QosFlowSetupRequestList`、PFCP `CreatePDR/CreateQER` 都已建立多個 flow-level 資訊，實際 GTP-U PDU Session Container 仍可能全部寫入 QFI 1。

根因不在於 dedicated PDR 同時關聯 `QER ID=1`。在 PFCP 語意上，同一個 PDR 可以同時參照多個 QER，例如：

```text
PDR 5 -> QER ID=1, QFI=1   session AMBR QER
      -> QER ID=4, QFI=3   10.200.0.2/32 dedicated QoS Flow QER
```

其中 `QER ID=1` 是 session-level AMBR QER，用來做整個 PDU Session 的速率控制；它可以存在於 dedicated PDR。真正的問題是 UPF / gtp5g 在要寫入 GTP-U PDU Session Container 的 QFI 時，不應該直接拿第一個有 QFI 的 QER。

原本 gtp5g 的 `set_pdr_qfi()` 會按照 PDR 內 `qer_ids` 的順序尋找第一個 `qer->qfi > 0`，找到後就寫入 `pdr->qfi` 並停止。因此若 PDR 的 QER 順序是：

```text
QER ID=1, QFI=1
QER ID=4, QFI=3
```

gtp5g 會先看到 `QER ID=1`，導致該 PDR 的 `pdr->qfi` 被設定成 1。這不是 PCF 沒建 Flow Rule，也不是 NGAP 沒帶 QoS Flow，而是 UPF / gtp5g 對「多 QER PDR」的 QFI 選擇策略錯誤。

最終修正原則如下：

1. PDR precedence 仍由 gtp5g 既有 PDR lookup 決定；也就是先依 precedence 找到真正命中的 PDR。
2. 命中的 PDR 內可以保留 session AMBR QER。
3. 寫入 GTP-U PDU Session Container 的 QFI 時，優先選擇該 PDR 內 `QFI != 1` 的 QER。
4. 若該 PDR 只有 default/session QER，才 fallback 到第一個非 0 QFI，通常是 QFI 1。

也就是：

```text
PDR precedence 決定哪條 Flow Rule 命中
PDR 內 QER 選擇決定 GTP-U 寫入哪個 QFI
```

目前修正位置：

- `NFs/smf/internal/context/datapath.go`
  - `ActivateTunnelAndPDR()` 與 `ActivateDcTunnelAndPDR()` 中，`defaultQER` 只掛在 `dataPath.IsDefaultPath`。
  - `ambrQER` 仍保留在 non-GBR PDR，避免失去 session AMBR enforcement。
- `NFs/upf/internal/forwarder/gtp5g.go`
  - 新增 `selectQERForPDRQFI()`。
  - userspace UPF 在 buffered packet 轉送時，優先選 `QFI != 1` 的 QER；找不到 dedicated QER 才 fallback 到 QFI 1。
- `gtp5g/src/genl/genl_pdr.c`
  - `set_pdr_qfi()` 改為優先選 `qer->qfi != 1`。
  - 若沒有非 1 QFI，才使用第一個非 0 QFI 作為 fallback。

修正後，若 SMF QFI 分配順序已依 `qosRef` 固定，以下對應關係會保留：

```text
QER ID 1 -> QFI 1   session AMBR QER
QER ID 2 -> QFI 1   default QoS Flow QER
QER ID 3 -> QFI 2   10.200.0.2/32 Flow Rule
QER ID 4 -> QFI 3   10.200.0.3/32 Flow Rule
QER ID 5 -> QFI 4   0.0.0.0/0 Flow Rule
```

因此 `QER ID=1` 仍然是 `QFI=1`，也仍然可以掛在 dedicated PDR；只是 dedicated PDR 的 GTP-U QFI 不再由 `QER ID=1` 決定，而是由該 PDR 內的 flow-level QER 決定。

### 10.8 上行 N3 QFI 仍是 1，但 gtp5g 可以正確打 skb mark

上行實驗中觀察到：

```text
UE iperf 到不同 Flow Rule filter 的 dst
N3 tcpdump 的 GTP-U PDU Session Container QFI 仍都是 1
但 gtp5g 解封裝後 skb->mark 可以是 2 / 3 / 4
```

這是合理現象，因為這兩個值來自不同階段：

- N3 pcap 看到的是 UE / gNB 送到 UPF 前已經寫好的 uplink GTP-U extension header QFI。
- `skb->mark` 是 UPF / gtp5g 收到封包後，解封裝並依 inner IP、Access PDR、SDF filter、flow-specific QER 重新分類後設定的 kernel metadata。

修正後的上行資料路徑為：

```text
N3 uplink GTP-U packet
  PDU Session Container QFI = 1
        ↓
gtp5g 解析 inner IP
        ↓
pdr_find_by_gtp1u() 依 TEID + UE IP + SDF filter 找 Access PDR
        ↓
命中 flow-specific Access PDR
        ↓
使用該 PDR 的 pdr->qfi
        ↓
gtp5g_apply_skb_label()
        ↓
skb->priority = QFI
skb->mark     = QFI
```

修正位置：

- `gtp5g/src/gtpu/encap.c`
  - `gtp5g_apply_skb_label(skb, pdr->qfi ? pdr->qfi : qfi)`
  - 優先使用命中的 Access PDR 的 `pdr->qfi`。
  - 只有 PDR 沒有 QFI 時，才 fallback 到 N3 PDU Session Container 原本解析出的 QFI。

因此：

```text
N3 tcpdump QFI=1
不代表 UPF 內部分類結果也是 1

gtp5g skb->mark=2/3/4
代表 UPF 解封裝後 flow-level Access PDR 已命中
```

若要讓 N3 uplink pcap 本身也看到 QFI 2/3/4，需修 UE / gNB 側 uplink QoS rule 或 packet filter 分類，讓 UE / gNB 送上行 GTP-U 時就標 dedicated QFI。UPF 端補做 flow-level 分類只能影響解封裝後的 `skb->mark` / `skb->priority` 與後續 TC 分流，不會回頭改寫已經抵達 UPF 的 N3 uplink header。

### 10.9 下行 skb mark 在 enp0s8 egress 前被清掉

下行實驗中也觀察到：

```text
dmesg 可看到 gtp5g 已設定：
gtp5g: qfi=2 priority=2 mark=0x2

但 enp0s8 egress qdisc 的 fw filter 看不到 skb->mark
```

追查後確認不是 TC 介面錯、不是封包沒經過 enp0s8、也不是 PDR / QER / QFI 錯，而是下行送出 tunnel 時 kernel tunnel transmit path 把 skb metadata scrub 掉。

問題流程為：

```text
gtp5g_fwd_skb_ipv4()
  -> gtp5g_push_header()
  -> gtp5g_apply_skb_label()
       skb->mark = QFI
  -> gtp5g_dev_xmit()
  -> gtp5g_xmit_skb_ipv4()
  -> udp_tunnel_xmit_skb(..., xnet=true, ...)
  -> iptunnel_xmit()
  -> skb_scrub_packet(skb, true)
       skb->mark = 0
  -> ip_local_out()
  -> enp0s8 egress qdisc
```

根因在 `gtp5g/src/gtpu/pktinfo.c` 的 `gtp5g_xmit_skb_ipv4()`：原本呼叫 `udp_tunnel_xmit_skb()` 時把 `xnet` 參數硬寫成 `true`。Linux tunnel path 在 `xnet == true` 時會呼叫 `skb_scrub_packet(skb, true)`，而這會清掉 `skb->mark`。

修正方式是改成依實際 netns 判斷：

```c
bool xnet = !net_eq(sock_net(pktinfo->sk), dev_net(pktinfo->dev));

udp_tunnel_xmit_skb(..., xnet, ...);
```

也就是只有 socket netns 與 gtp5g device netns 不同時，才視為 cross-netns tunnel transmit。一般 UPF socket 與 gtp5g device 在同一個 netns 時，`xnet=false`，`skb->mark` 就能保留到實體介面 egress qdisc。

### 10.10 上下行統一使用 QFI-based skb label

原本 gtp5g 曾經有針對特定 TEID 的實驗性 mark：

```text
TEID 2 + QFI 1 -> mark 0x109
TEID 6 + QFI 1 -> mark 0x208
```

後續已移除這些 TEID-specific 特例，改成上下行共用同一個 QFI-based label 規則：

```text
QFI=N -> skb->priority=N, skb->mark=N
QFI=0 -> skb->priority=0, skb->mark=0
```

修正位置：

- `gtp5g/src/gtpu/encap.c`
  - `gtp5g_skb_label()`
  - `gtp5g_apply_skb_label()`
  - 上行解封裝後套用 `pdr->qfi ? pdr->qfi : qfi`
  - 下行封裝後 / 送出前套用 `pdr->qfi`

因此 TC 規則可以直接依 `skb->mark` 或 `skb->priority` 使用 QFI 值分流，不需要再理解 TEID 與實驗 mark 的對應。

## 11. 建議追查順序

1. PCF log 或 PCF response：確認 `QosDecs` 有三筆，`PccRules` 有三條 flow-level rules，且每條有 `RefQosData`。
2. SMF log：確認有 `Install PCCRule`、`AssignQFI` 後的非 1 QFI，以及 `AddQoS called: QFI=2/3/4` 類似訊息。
3. NGAP pcap：看 `PDUSessionResourceSetupRequestTransfer` 的 `QosFlowSetupRequestList` 是否包含 QFI 1 以外的 QFI。
4. NAS pcap：看 `PDUSessionEstablishmentAccept` 的 Authorized QoS Rules / QoS Flow Descriptions 是否包含 flow-level QFI。
5. PFCP pcap：看 `CreateQER.QoSFlowIdentifier` 是否有非 1 QFI，且 flow-level `CreatePDR` 是否帶 `SDF Filter` 並參照該 QERID。
6. UPF 實際分類：確認封包五元組符合 SDF filter；若不符合，仍會命中 default PDR/QER/QFI 1。
7. 上行 TC 分流：若 N3 uplink pcap 仍顯示 QFI 1，但 `skb->mark` 是 2/3/4，代表 UPF 解封裝後分類生效；這不等於 UE / gNB 已正確標上行 QFI。
8. 下行 TC 分流：若 gtp5g printk 顯示 `mark=0x2` 但 egress qdisc 看不到 mark，檢查 `gtp5g_xmit_skb_ipv4()` 是否仍把 `udp_tunnel_xmit_skb()` 的 `xnet` 硬設成 `true`。

## 12. 程式碼索引

| 階段 | 檔案 | 重點 |
| --- | --- | --- |
| webconsole 表單 | `webconsole/frontend/src/pages/SubscriberCreate/FormFlowRule.tsx` | Flow Rule 欄位 |
| DTO 轉換 | `webconsole/frontend/src/lib/dtos/subscription.ts` | `qosRef` 產生，拆成 `FlowRules` / `QosFlows` |
| 後端 model | `webconsole/backend/WebUI/model_flow_rule.go`、`model_qos_flow.go` | Mongo 文件欄位 |
| Mongo 寫入 | `webconsole/backend/WebUI/api_webui.go` | 寫入 `policyData.ues.flowRule` / `qosFlow` |
| PCF policy | `NFs/pcf/internal/sbi/processor/smpolicy.go` | Mongo 讀取、建立 `QosDecs` / `PccRules` |
| PCF PCC util | `NFs/pcf/internal/util/pcc_rule.go` | default PCC rule、`SetPccRuleRelatedByQosRef()` |
| SMF PCF client | `NFs/smf/internal/sbi/consumer/pcf_service.go` | 建立 `SmPolicyContextData` |
| SMF 套用 policy | `NFs/smf/internal/context/sm_context_policy.go` | `ApplyPccRules()`、依 `RefQosData` 固定 QFI 配置順序 |
| SMF policy 測試 | `NFs/smf/internal/context/sm_context_policy_internal_test.go` | 驗證 PCC rules 依 `qosRef` 1、2、3 排序 |
| SMF QFI generator | `NFs/smf/internal/context/sm_context.go` | QFI 從 2 開始，1 保留 default |
| DataPath/PDR/QER | `NFs/smf/internal/context/datapath.go` | `ActivateTunnelAndPDR()`、`AddQoS()`、`UpdateFlowDescription()` |
| PFCP message | `NFs/smf/internal/pfcp/message/build.go` | `CreatePDR` / `CreateQER` |
| UPF userspace QER 選擇 | `NFs/upf/internal/forwarder/gtp5g.go` | `selectQERForPDRQFI()`、buffered packet 轉送時選擇 GTP-U QFI |
| gtp5g kernel QFI 寫入 | `gtp5g/src/genl/genl_pdr.c` | `set_pdr_qfi()`，依 PDR 內 QER 選出 `pdr->qfi` |
| gtp5g skb label | `gtp5g/src/gtpu/encap.c` | 上下行依 QFI 設定 `skb->priority` / `skb->mark` |
| gtp5g tunnel xmit | `gtp5g/src/gtpu/pktinfo.c` | `udp_tunnel_xmit_skb()` 的 `xnet` 依 netns 判斷，避免 mark 被 scrub |
| NGAP transfer | `NFs/smf/internal/context/ngap_build.go` | `QosFlowSetupRequestList` |
| NAS QoS | `NFs/smf/internal/context/gsm_build.go` | Authorized QoS Rules / QoS Flow Descriptions |
| AMF N1N2 | `NFs/amf/internal/sbi/processor/n1n2message.go` | N2 SM information 轉 NGAP |
| AMF NGAP | `NFs/amf/internal/ngap/message/build.go`、`send.go` | `PDUSessionResourceSetupRequest` |
