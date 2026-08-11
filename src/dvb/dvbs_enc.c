/*
 * DVB-S outer coding in portable C.
 *
 * Replaces dvbsenco8.s, which is hand-written 32-bit ARM and therefore does
 * not build on a 64-bit Pi. The assembly was written in 2013 to hit 15 us per
 * packet on a 700 MHz Pi 1; there is no performance reason to keep it on a
 * 2.4 GHz Cortex-A76, and these are all standard published algorithms.
 *
 * Per ETSI EN 300 421, in order:
 *   4.4  transport multiplex adaptation and energy dispersal (PRBS)
 *   4.5  outer coding, RS(204,188) shortened from RS(255,239)
 *   4.6  convolutional interleaving, I = 12, M = 17
 *
 * Same entry points as the assembly: dvbsenco_init() then dvbsenco() per
 * 188-byte transport packet, returning a 204-byte interleaved packet.
 */

#include <string.h>
#include <stdint.h>

typedef unsigned char uchar;

/* ---------------- 4.4 energy dispersal ----------------
 *
 * PRBS polynomial 1 + x^14 + x^15, reloaded with 100101010000000 at the start
 * of every 8 packets. The sync byte of the first packet is inverted to mark
 * that point. The generator keeps clocking through the other seven sync
 * bytes but its output is not applied to them, so the sequence period is
 * 8*188 - 1 = 1503 bytes.
 */

#define PRBS_INIT 0x4A80        /* 100101010000000 in the low 15 bits */

static uint16_t prbs_reg;
static int group_byte;          /* 0..1503 within the 8-packet group */

static uchar prbs_byte(void)
{
	uchar out = 0;
	int i;

	for (i = 0; i < 8; i++) {
		/* Stage k lives at bit (15-k), so stage 1 is bit 14 and stage 15
		 * is bit 0. The output is s14 ^ s15 and is fed back into s1,
		 * which in this packing is a right shift with the new bit at
		 * bit 14. Bits are taken MSB first. */
		uint16_t bit = (uint16_t)(((prbs_reg >> 1) ^ prbs_reg) & 1);
		out = (uchar)((out << 1) | bit);
		prbs_reg = (uint16_t)((prbs_reg >> 1) | (bit << 14));
	}
	return out;
}

static void energy_reset(void)
{
	prbs_reg = PRBS_INIT;
	group_byte = 0;
}

/* Randomise one 188-byte packet in place into `out`. */
static void energy_packet(const uchar *in, uchar *out)
{
	int i;

	for (i = 0; i < 188; i++) {
		if (group_byte == 0) {
			/* start of a group: reload, invert sync, do not clock */
			prbs_reg = PRBS_INIT;
			out[i] = 0xB8;
		} else if ((group_byte % 188) == 0) {
			/* sync byte of packets 2..8: clocked but not applied */
			(void)prbs_byte();
			out[i] = 0x47;
		} else {
			out[i] = (uchar)(in[i] ^ prbs_byte());
		}
		if (++group_byte >= 8 * 188)
			group_byte = 0;
	}
}

/* ---------------- 4.5 RS(204,188) ----------------
 *
 * GF(256) with p(x) = x^8 + x^4 + x^3 + x^2 + 1, and the code generator
 * g(x) = prod_{i=0..15} (x + a^i) with a = 0x02. Shortened from RS(255,239)
 * by prepending 51 implicit zero bytes, which for a systematic encoder just
 * means feeding the 188 bytes straight in.
 */

#define GF_POLY 0x11D
#define RS_NPAR 16

static uchar gf_exp[512];
static uchar gf_log[256];
static uchar rs_gen[RS_NPAR + 1];

static void gf_init(void)
{
	int i;
	unsigned x = 1;

	for (i = 0; i < 255; i++) {
		gf_exp[i] = (uchar)x;
		gf_log[x] = (uchar)i;
		x <<= 1;
		if (x & 0x100)
			x ^= GF_POLY;
	}
	for (i = 255; i < 512; i++)
		gf_exp[i] = gf_exp[i - 255];
	gf_log[0] = 0;          /* never used; log(0) is undefined */
}

static uchar gf_mul(uchar a, uchar b)
{
	if (!a || !b)
		return 0;
	return gf_exp[gf_log[a] + gf_log[b]];
}

static void rs_init(void)
{
	int i, j;

	gf_init();
	memset(rs_gen, 0, sizeof(rs_gen));
	rs_gen[0] = 1;
	/* multiply out (x + a^i) for i = 0..15 */
	for (i = 0; i < RS_NPAR; i++) {
		for (j = i; j >= 0; j--) {
			rs_gen[j + 1] ^= gf_mul(rs_gen[j], gf_exp[i]);
		}
	}
}

/* in: 188 bytes, out: 204 bytes (systematic, parity appended) */
static void rs_encode(const uchar *in, uchar *out)
{
	uchar par[RS_NPAR];
	int i, j;

	memcpy(out, in, 188);
	memset(par, 0, sizeof(par));

	for (i = 0; i < 188; i++) {
		uchar fb = in[i] ^ par[0];
		for (j = 0; j < RS_NPAR - 1; j++)
			par[j] = par[j + 1] ^ gf_mul(fb, rs_gen[j + 1]);
		par[RS_NPAR - 1] = gf_mul(fb, rs_gen[RS_NPAR]);
	}
	memcpy(out + 188, par, RS_NPAR);
}

/* ---------------- 4.6 convolutional interleaver ----------------
 *
 * 12 branches; branch j is a FIFO of j*17 bytes, branch 0 a straight wire.
 * Bytes go to branches cyclically and the sync byte always lands on branch 0,
 * which is what keeps the deinterleaver aligned. Because of the delay lines
 * the first 11 output packets are only partly filled - the assembly's header
 * notes the same thing.
 */

#define INTER_I 12
#define INTER_M 17

static uchar inter_fifo[INTER_I][INTER_I * INTER_M];   /* branch 0 unused */
static int inter_pos[INTER_I];
static int inter_branch;

static void interleave_reset(void)
{
	memset(inter_fifo, 0, sizeof(inter_fifo));
	memset(inter_pos, 0, sizeof(inter_pos));
	inter_branch = 0;
}

static uchar interleave_byte(uchar in)
{
	int b = inter_branch;
	int depth = b * INTER_M;
	uchar out;

	if (depth == 0) {
		out = in;
	} else {
		int p = inter_pos[b];
		out = inter_fifo[b][p];
		inter_fifo[b][p] = in;
		if (++p >= depth)
			p = 0;
		inter_pos[b] = p;
	}
	if (++inter_branch >= INTER_I)
		inter_branch = 0;
	return out;
}

/* ---------------- public API (same as the assembly) ---------------- */

static uchar work_rand[188];
static uchar work_rs[204];
static uchar work_out[204];

void dvbsenco_init(void)
{
	energy_reset();
	rs_init();
	interleave_reset();
}

uchar *dvbsenco(uchar *packetin)
{
	int i;

	energy_packet(packetin, work_rand);
	rs_encode(work_rand, work_rs);
	for (i = 0; i < 204; i++)
		work_out[i] = interleave_byte(work_rs[i]);
	return work_out;
}

/* The assembly exported these separately as well; dvbrf.cpp declares them,
 * so keep them available for anything that calls the stages directly. */
/* The assembly's standalone energy() restarts the sequence on every call
 * (dvbsenco keeps the real 8-packet group state internally), so match that. */
void energy(uchar *input, uchar *output)
{
	energy_reset();
	energy_packet(input, output);
}

void reed(uchar *input188)
{
	uchar tmp[204];

	rs_encode(input188, tmp);
	memcpy(input188, tmp, 204);
}

uchar *interleave(uchar *packetin)
{
	int i;

	for (i = 0; i < 204; i++)
		work_out[i] = interleave_byte(packetin[i]);
	return work_out;
}
