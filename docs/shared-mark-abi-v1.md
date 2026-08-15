# gtp5g shared `skb->mark` ABI v1

## Status

The kernel runtime implements shared mark ABI v1. PDRs can carry an RCU-safe
FlowQoS binding, decimal FAR Forwarding Policy Identifiers are validated as
8-bit route IDs, and the N6, N9 and downlink N3 paths use one helper to set the
packet metadata. go-upf/go-gtp5gnl support for publishing the new PDR attribute
is the next integration step.

`GTP5G_CMD_GET_VERSION` also returns `GTP5G_SHARED_MARK_ABI = 1` so user space
can require this capability before publishing FlowQoS bindings. The existing
driver-version attribute and its number are unchanged.

## Layout

```text
31                         24 23                          0
+----------------------------+-----------------------------+
| MEC routing ID (8 bits)    | FlowQoS policy ID (24 bits) |
+----------------------------+-----------------------------+
```

Definitions are provided by `include/gtp5g_mark.h`:

```c
GTP5G_ROUTE_MARK_MASK   = 0xff000000
GTP5G_QOS_MARK_MASK     = 0x00ffffff
GTP5G_MARK_ABI_VERSION  = 1
```

ID 0 means that the corresponding subsystem has no explicit policy:

- route ID 0 selects the default route;
- FlowQoS policy ID 0 selects the default/unmanaged QoS behavior.

The v1 layout assigns all 32 mark bits to gtp5g. Any future mark consumer must
be assigned bits through an explicit ABI revision.

## Examples

| MEC route ID | FlowQoS policy ID | Composed mark |
|---:|---:|---:|
| 0 | 0 | `0x00000000` |
| 10 | 0 | `0x0a000000` |
| 0 | 1001 | `0x000003e9` |
| 10 | 1001 | `0x0a0003e9` |

## Consumer masks

Linux policy routing must only inspect the route field. For route ID 10:

```bash
ip rule add pref 100 \
  fwmark 0x0a000000/0xff000000 \
  table 100
```

TC/eBPF must only use the FlowQoS field as its policy-map key:

```c
policy_id = skb->mark & GTP5G_QOS_MARK_MASK;
```

`skb->priority` is not part of this bit layout. When a valid FlowQoS binding is
present it carries the complete Linux TC classid. QFI remains a separate 5G
protocol value used in the GTP-U PDU Session Container.

## PDR FlowQoS netlink binding

`GTP5G_PDR_FLOW_QOS` is appended to the PDR Generic Netlink attributes. It is
a nested attribute with this contract:

| Nested attribute | Type | Meaning |
|---|---|---|
| `GTP5G_FLOW_QOS_VERSION` | `u8` | Must equal `1` |
| `GTP5G_FLOW_QOS_POLICY_ID` | `u32` | Low 24-bit policy-map key |
| `GTP5G_FLOW_QOS_TC_CLASSID` | `u32` | Complete Linux TC classid |
| `GTP5G_FLOW_QOS_FLAGS` | `u8` | Bit 0 is `GTP5G_FLOW_QOS_VALID` |
| `GTP5G_FLOW_QOS_GENERATION` | `u32` | Control-plane generation |

Version and flags are mandatory. A valid binding also requires policy ID and
TC classid. Sending flags `0` clears an existing binding. Create, replace,
get and dump PDR operations use the same nested representation. Unsupported
versions, unknown flags and policy IDs above `0x00ffffff` are rejected.

Bindings are immutable after publication. PDR replacement swaps the pointer
with RCU and retires the old object after a grace period, so a packet cannot
observe fields from two different generations.

## Packet paths

| Path | Route field | FlowQoS field and classid | Write point |
|---|---|---|---|
| uplink N3 to N6 | FAR route ID | PDR binding | after decapsulation |
| uplink N9 forwarding | FAR route ID | PDR binding | before `ip_xmit()` |
| downlink N6 to N3 | zero | PDR binding | after GTP-U header creation |

The downlink N3 path deliberately does not apply the FAR route ID to the outer
packet destined for the RAN. This preserves the forwarding-policy scope used
by the prior implementation.

Until go-upf publishes `GTP5G_PDR_FLOW_QOS`, the packet path uses QFI as a
legacy fallback for both the low mark field and `skb->priority`. This keeps the
current QFI-based experiment working, but it is not MBR/GBR enforcement and
must be removed after the control-plane integration is deployed.

## Validation contract

Control-plane callers must reject values outside these ranges:

```text
route ID:          0 .. 255
FlowQoS policy ID: 0 .. 16777215
```

`gtp5g_mark_compose()` masks both inputs as a containment guard, but masking is
not a substitute for validation. Silently truncating an invalid ID could bind a
packet to a different routing or QoS policy.

## Legacy migration

The old packet path treated a decimal Forwarding Policy Identifier as a
complete mark. For example, identifier `10` previously produced mark
`0x0000000a`.

With ABI v1, identifier `10` represents route ID 10 and produces route bits
`0x0a000000`. Existing `ip rule` entries must
therefore be migrated from an unmasked value such as:

```bash
ip rule add fwmark 10 table 100
```

to the masked ABI v1 form:

```bash
ip rule add fwmark 0x0a000000/0xff000000 table 100
```

The runtime switch must be delivered together with the route-rule migration;
otherwise MEC traffic will fall through to the default route.


## Tests

Run the host-side ABI tests without loading the kernel module:

```bash
make test
```

The tests cover field limits, the required route/QoS matrix, round-trip
decoding and containment of invalid inputs.

After loading the module, PDR and FAR proc snapshots expose the resolved QFI,
FlowQoS version/flags/policy/classid/generation, route ID and an N6/N9 composed
mark example. Use the existing proc selection interface for the target SEID and
rule ID; packet-level `printk` is intentionally not used.

The kernel build validates the netlink parser and RCU integration:

```bash
make
```
