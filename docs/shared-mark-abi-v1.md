# gtp5g shared `skb->mark` ABI v1

## Status

This document defines the first implementation phase of the shared mark ABI.
Phase 1 adds the ABI constants, encode/decode helpers, validation helpers and
host-side tests. It deliberately does not change the current packet-path mark
writes yet.

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

`skb->priority` is not part of this bit layout. A later phase will use it for
the complete Linux TC classid, while QFI remains a separate 5G protocol value.

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

The current packet path treats a decimal Forwarding Policy Identifier as a
complete mark. For example, identifier `10` currently produces mark
`0x0000000a`.

When the packet path is migrated to ABI v1, identifier `10` will represent
route ID 10 and produce route bits `0x0a000000`. Existing `ip rule` entries must
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

The current QFI-based `skb->mark = qfi` behavior is also legacy behavior. A
later phase will replace it with a PDR FlowQoS binding and compose the mark once
from the matched PDR and FAR.

## Tests

Run the host-side ABI tests without loading the kernel module:

```bash
make test
```

The tests cover field limits, the required route/QoS matrix, round-trip
decoding and containment of invalid inputs.
