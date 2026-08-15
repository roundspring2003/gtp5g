/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __GTP5G_MARK_H__
#define __GTP5G_MARK_H__

#include <linux/types.h>

/*
 * gtp5g skb mark ABI version 1
 *
 *  31                         24 23                          0
 * +----------------------------+-----------------------------+
 * | MEC routing ID (8 bits)    | FlowQoS policy ID (24 bits) |
 * +----------------------------+-----------------------------+
 *
 * ID 0 in either field means that no explicit policy is selected for
 * that subsystem.
 */
#define GTP5G_MARK_ABI_VERSION       1U

#define GTP5G_ROUTE_MARK_MASK        0xff000000U
#define GTP5G_ROUTE_MARK_SHIFT       24U
#define GTP5G_ROUTE_ID_MAX           \
	(GTP5G_ROUTE_MARK_MASK >> GTP5G_ROUTE_MARK_SHIFT)

#define GTP5G_QOS_MARK_MASK          0x00ffffffU
#define GTP5G_QOS_MARK_SHIFT         0U
#define GTP5G_QOS_POLICY_ID_MAX      \
	(GTP5G_QOS_MARK_MASK >> GTP5G_QOS_MARK_SHIFT)

#define GTP5G_OWNED_MARK_MASK        \
	(GTP5G_ROUTE_MARK_MASK | GTP5G_QOS_MARK_MASK)

#if (GTP5G_ROUTE_MARK_MASK & GTP5G_QOS_MARK_MASK)
#error "gtp5g route and FlowQoS mark fields overlap"
#endif

#if (GTP5G_OWNED_MARK_MASK != 0xffffffffU)
#error "gtp5g mark ABI v1 must define ownership for all 32 mark bits"
#endif

static inline int gtp5g_route_id_is_valid(__u32 route_id)
{
	return route_id <= GTP5G_ROUTE_ID_MAX;
}

static inline int gtp5g_qos_policy_id_is_valid(__u32 policy_id)
{
	return policy_id <= GTP5G_QOS_POLICY_ID_MAX;
}

/*
 * Callers must validate both IDs before composing a mark. Masking here is a
 * final containment guard, not a substitute for rejecting invalid control
 * plane values.
 */
static inline __u32 gtp5g_mark_compose(__u32 route_id, __u32 policy_id)
{
	return ((route_id << GTP5G_ROUTE_MARK_SHIFT) &
		 GTP5G_ROUTE_MARK_MASK) |
	       ((policy_id << GTP5G_QOS_MARK_SHIFT) &
		 GTP5G_QOS_MARK_MASK);
}

static inline __u32 gtp5g_mark_route_id(__u32 mark)
{
	return (mark & GTP5G_ROUTE_MARK_MASK) >> GTP5G_ROUTE_MARK_SHIFT;
}

static inline __u32 gtp5g_mark_qos_policy_id(__u32 mark)
{
	return (mark & GTP5G_QOS_MARK_MASK) >> GTP5G_QOS_MARK_SHIFT;
}

#endif /* __GTP5G_MARK_H__ */
