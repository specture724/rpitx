/*
 * DVB-S2 is implemented in dvbs2arm_1v30.s, 2700 lines of hand-written 32-bit
 * ARM assembly (BCH + LDPC for every code rate). It cannot be assembled for
 * aarch64 and there is no C equivalent in the tree, so on 64-bit builds the
 * DVB-S2 mode reports that it is unavailable rather than silently producing
 * nothing. DVB-S is unaffected - dvbs_enc.c replaces the other assembly file.
 */

#include <stdio.h>
#include <stdint.h>

typedef unsigned char uchar;

static void complain(void)
{
	static int said;

	if (said)
		return;
	said = 1;
	fprintf(stderr,
		"dvbrf: DVB-S2 is not available on this build.\n"
		"       Its encoder is 32-bit ARM assembly (dvbs2arm_1v30.s) with no\n"
		"       C equivalent, so it cannot be built for aarch64.\n"
		"       Use -m dvbs for DVB-S, which is fully supported.\n");
}

uint32_t _dvbs2arm_control(uint32_t command, uintptr_t param1)
{
	(void)command;
	(void)param1;
	complain();
	return 0;
}

uchar *_dvbs2arm_process_packet(uchar *packet188)
{
	(void)packet188;
	complain();
	return 0;
}

void _reverse_byte_order(void)
{
	complain();
}
