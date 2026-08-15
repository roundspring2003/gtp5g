/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "gtp5g_mark.h"

static void test_field_limits(void)
{
	assert(GTP5G_MARK_ABI_VERSION == 1U);
	assert(GTP5G_ROUTE_ID_MAX == 0xffU);
	assert(GTP5G_QOS_POLICY_ID_MAX == 0xffffffU);

	assert(gtp5g_route_id_is_valid(0U));
	assert(gtp5g_route_id_is_valid(GTP5G_ROUTE_ID_MAX));
	assert(!gtp5g_route_id_is_valid(GTP5G_ROUTE_ID_MAX + 1U));

	assert(gtp5g_qos_policy_id_is_valid(0U));
	assert(gtp5g_qos_policy_id_is_valid(GTP5G_QOS_POLICY_ID_MAX));
	assert(!gtp5g_qos_policy_id_is_valid(
		GTP5G_QOS_POLICY_ID_MAX + 1U));
}

static void test_mark_matrix(void)
{
	assert(gtp5g_mark_compose(0U, 0U) == 0x00000000U);
	assert(gtp5g_mark_compose(10U, 0U) == 0x0a000000U);
	assert(gtp5g_mark_compose(0U, 1001U) == 0x000003e9U);
	assert(gtp5g_mark_compose(10U, 1001U) == 0x0a0003e9U);
	assert(gtp5g_mark_compose(
		GTP5G_ROUTE_ID_MAX,
		GTP5G_QOS_POLICY_ID_MAX) == UINT32_MAX);
}

static void test_round_trip(void)
{
	static const __u32 route_ids[] = { 0U, 1U, 10U, 127U, 255U };
	static const __u32 policy_ids[] = {
		0U, 1U, 1001U, 0x7fffffU, 0xffffffU
	};
	size_t route_index;
	size_t policy_index;

	for (route_index = 0;
	     route_index < sizeof(route_ids) / sizeof(route_ids[0]);
	     route_index++) {
		for (policy_index = 0;
		     policy_index < sizeof(policy_ids) / sizeof(policy_ids[0]);
		     policy_index++) {
			__u32 mark = gtp5g_mark_compose(
				route_ids[route_index], policy_ids[policy_index]);

			assert(gtp5g_mark_route_id(mark) ==
			       route_ids[route_index]);
			assert(gtp5g_mark_qos_policy_id(mark) ==
			       policy_ids[policy_index]);
		}
	}
}

static void test_invalid_values_are_contained(void)
{
	__u32 mark;

	/*
	 * Invalid IDs must be rejected by control-plane callers. The compose
	 * helper still masks them so one field can never corrupt the other.
	 */
	mark = gtp5g_mark_compose(0x10aU, 0x10003e9U);
	assert(mark == 0x0a0003e9U);
	assert(gtp5g_mark_route_id(mark) == 10U);
	assert(gtp5g_mark_qos_policy_id(mark) == 1001U);
}

int main(void)
{
	test_field_limits();
	test_mark_matrix();
	test_round_trip();
	test_invalid_values_are_contained();

	puts("gtp5g shared skb mark ABI tests: PASS");
	return 0;
}
