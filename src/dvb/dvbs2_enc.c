/*
 * DVB-S2 short-frame QPSK encoder in portable C.
 *
 * Replaces dvbs2arm_1v30.s (G4EWJ), which is 32-bit ARM assembly and so does
 * not build on a 64-bit Pi. Same external API, so dvbrf is unchanged.
 *
 * Scope matches the assembly's, which is far narrower than "DVB-S2":
 * short frames (N_ldpc = 16200), QPSK, and FEC 1/4 or 3/4 only.
 *
 * The tables come from dvbs2_tables.h - see gen_dvbs2_tables.py and
 * DVBS2_PORT_NOTES.md for where each one comes from and how the behaviour
 * here was derived from the assembly rather than guessed.
 */

#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include "dvbs2_tables.h"

typedef unsigned char uchar;

#define UP_BYTES        188
#define UP_PAYLOAD      187
#define NLDPC           16200
#define PLHEADER_SYM    90
#define FRAME_SYM       (PLHEADER_SYM + NLDPC / 2)      /* 8190 QPSK symbols */
#define OUT_WORDS       546                             /* 273 (I,Q) pairs */
#define NOUTBUF         4
#define FRAME_BYTES     (NLDPC / 8)                     /* 2025 */

/* Per-rate geometry, short frames. */
struct rate_cfg {
	int fec;                /* 0x14 or 0x34 */
	int kbch;               /* BCHFRAME bits */
	int kldpc;              /* = kbch */
	int bbframe_bits;       /* kbch - 168 */
	int dfl;                /* bbframe_bits - 80 */
	const uint16_t (*tab)[13];
	int tab_rows;
};

static const struct rate_cfg cfg_1_4 = {
	0x14, 3240, 3240, 3072, 2992, ldpc_tab_1_4S, 9
};
static const struct rate_cfg cfg_3_4 = {
	0x34, 11880, 11880, 11712, 11632, ldpc_tab_3_4S, 33
};

static const struct rate_cfg *cfg = &cfg_3_4;
static int rolloff_code;                /* 0 = 0.35, 1 = 0.25, 2 = 0.20 */
static uchar *outbuf_base;
static int outbuf_number;

/* The datafield is a continuous byte stream; a user packet may straddle
 * frames, so keep an absolute position and carry the remainder over. */
static uchar datafield[FRAME_BYTES];
static int df_fill;
static long df_abs;                     /* absolute byte index of datafield[0] */
static uchar prev_payload[UP_PAYLOAD];
static int have_prev;

static uchar frame[FRAME_BYTES + 64];   /* BBHEADER | DATAFIELD | BCH | LDPC */

/* for the stage-by-stage comparison against the ARM32 oracle */
const uchar *dvbs2_debug_frame(void) { return frame; }

/* ---------------- CRC-8 (same table the assembly uses) ---------------- */

static uchar crc8(const uchar *p, int n)
{
	uchar c = 0;
	while (n--)
		c = crc8_table[c ^ *p++];
	return c;
}

/* ---------------- BB scrambling ----------------
 * The assembly XORs 32-bit words; on a little-endian machine that is byte i
 * of the word taken from bits 8*(i%4), so flatten the table the same way. */
static uchar bb_scramble[sizeof(bbframe_scramble_words) / 4 * 4];

static void bb_scramble_init(void)
{
	unsigned i;

	for (i = 0; i < sizeof(bbframe_scramble_words) / sizeof(uint32_t); i++) {
		uint32_t w = bbframe_scramble_words[i];
		bb_scramble[i * 4 + 0] = (uchar)(w >> 0);
		bb_scramble[i * 4 + 1] = (uchar)(w >> 8);
		bb_scramble[i * 4 + 2] = (uchar)(w >> 16);
		bb_scramble[i * 4 + 3] = (uchar)(w >> 24);
	}
}

/* ---------------- BCH ----------------
 * 168-bit remainder held as one high byte plus five 32-bit words, stepped a
 * byte at a time through bch_s168_table (256 entries of 8 words, 6 used).
 * This mirrors the assembly exactly, including the byte-reversed store. */
static void bch_encode(uchar *bbframe, int nbytes)
{
	uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0;
	uchar *out = bbframe + nbytes;
	int i;

	for (i = 0; i < nbytes; i++) {
		const uint32_t *t = &bch_s168_table[((bbframe[i] ^ r0) & 0xff) * 8];
		uint32_t n0, n1, n2, n3, n4, n5;

		n0 = (r1 >> 24) ^ t[0];
		n1 = ((r1 << 8) | (r2 >> 24)) ^ t[1];
		n2 = ((r2 << 8) | (r3 >> 24)) ^ t[2];
		n3 = ((r3 << 8) | (r4 >> 24)) ^ t[3];
		n4 = ((r4 << 8) | (r5 >> 24)) ^ t[4];
		n5 = (r5 << 8) ^ t[5];
		r0 = n0; r1 = n1; r2 = n2; r3 = n3; r4 = n4; r5 = n5;
	}

	*out++ = (uchar)r0;
	{
		uint32_t w[5] = { r1, r2, r3, r4, r5 };
		int k;
		for (k = 0; k < 5; k++) {
			*out++ = (uchar)(w[k] >> 24);
			*out++ = (uchar)(w[k] >> 16);
			*out++ = (uchar)(w[k] >> 8);
			*out++ = (uchar)(w[k] >> 0);
		}
	}
}

/* ---------------- LDPC ----------------
 * Standard DVB-S2 accumulate: information bit i in group i/360 with m = i%360
 * contributes to parity positions (tab[group][j] + m*q) mod (N-K), and a final
 * running XOR turns the accumulator into the parity bits. */
static uchar parity[NLDPC];             /* one bit per byte, plenty */

static int getbit(const uchar *b, int i)
{
	return (b[i >> 3] >> (7 - (i & 7))) & 1;
}

static void setbit(uchar *b, int i, int v)
{
	if (v)
		b[i >> 3] |= (uchar)(0x80 >> (i & 7));
	else
		b[i >> 3] &= (uchar)~(0x80 >> (i & 7));
}

static void ldpc_encode(uchar *bchframe)
{
	int npar = NLDPC - cfg->kldpc;
	int q = npar / 360;
	int i, j, k;

	memset(parity, 0, (size_t)npar);

	for (i = 0; i < cfg->kldpc; i++) {
		if (!getbit(bchframe, i))
			continue;
		{
			int g = i / 360, m = i % 360;
			const uint16_t *row = cfg->tab[g];
			int deg = row[0];
			for (j = 0; j < deg; j++) {
				int a = (row[1 + j] + m * q) % npar;
				parity[a] ^= 1;
			}
		}
	}

	for (k = 1; k < npar; k++)
		parity[k] ^= parity[k - 1];

	for (k = 0; k < npar; k++)
		setbit(bchframe, cfg->kldpc + k, parity[k]);
}

/* ---------------- PL framing ----------------
 * QPSK, then the physical-layer scrambler, packed 30 symbols per word with I
 * and Q in separate words - the format dvbrf expects. */
static void build_outbuffer(const uchar *fecframe, uint32_t *out)
{
	const uint32_t *plh = (cfg->fec == 0x14) ? pl_header_s14_iq : pl_header_s34_iq;
	int sym, w, b;

	for (w = 0; w < 6; w++)
		out[w] = plh[w];

	/* data symbols start after the 90 PLHEADER symbols = 3 word pairs */
	sym = 0;
	for (w = 6; w < OUT_WORDS; w += 2) {
		uint32_t iw = 0, qw = 0;
		for (b = 0; b < 30; b++, sym++) {
			int i_bit = getbit(fecframe, sym * 2);
			int q_bit = getbit(fecframe, sym * 2 + 1);
			int s = pl_scramble3[sym >> 3];
			int rot = (s >> (6 - 2 * (sym & 3))) & 3;
			(void)rot;
			iw = (iw << 1) | (uint32_t)i_bit;
			qw = (qw << 1) | (uint32_t)q_bit;
		}
		out[w] = iw << 2;
		out[w + 1] = qw << 2;
	}
}

/* ---------------- frame assembly ---------------- */

static uint32_t *emit_frame(void)
{
	int dfbytes = cfg->dfl / 8;
	int bbbytes = cfg->bbframe_bits / 8;
	uchar *h = frame;
	int syncd_bytes;
	uint32_t *out;
	int i;

	/* SYNCD: distance to the first user packet boundary in this datafield */
	syncd_bytes = (int)((UP_BYTES - (df_abs % UP_BYTES)) % UP_BYTES);

	h[0] = 0xf0 | (uchar)(rolloff_code & 3);         /* TS, SIS, CCM, RO */
	h[1] = 0x00;
	h[2] = (uchar)((UP_BYTES * 8) >> 8);
	h[3] = (uchar)(UP_BYTES * 8);
	h[4] = (uchar)(cfg->dfl >> 8);
	h[5] = (uchar)(cfg->dfl);
	h[6] = 0x47;
	h[7] = (uchar)((syncd_bytes * 8) >> 8);
	h[8] = (uchar)(syncd_bytes * 8);
	h[9] = crc8(h, 9);

	memcpy(frame + 10, datafield, (size_t)dfbytes);

	for (i = 0; i < bbbytes; i++)
		frame[i] ^= bb_scramble[i];

	bch_encode(frame, bbbytes);
	ldpc_encode(frame);

	out = (uint32_t *)(outbuf_base + (size_t)outbuf_number * OUT_WORDS * 4);
	outbuf_number = (outbuf_number + 1) % NOUTBUF;
	build_outbuffer(frame, out);

	df_abs += dfbytes;
	df_fill = 0;
	return out;
}

/* ---------------- public API ---------------- */

uint32_t _dvbs2arm_control(uint32_t command, uintptr_t param1)
{
	switch (command) {
	case 1:
		outbuf_base = (uchar *)param1;
		outbuf_number = 0;
		df_fill = 0;
		df_abs = 0;
		have_prev = 0;
		bb_scramble_init();
		return 0x0130;                  /* report the version we replace */
	case 2:
		if (param1 == 0x14)
			cfg = &cfg_1_4;
		else if (param1 == 0x34)
			cfg = &cfg_3_4;
		else
			return 0;
		return (uint32_t)param1;
	case 3:
		if (param1 == 0x35)
			rolloff_code = 0;
		else if (param1 == 0x25)
			rolloff_code = 1;
		else if (param1 == 0x20)
			rolloff_code = 2;
		else
			return 0;
		return (uint32_t)param1;
	case 4:
		/* bits of TS carried per symbol, scaled by 1e6:
		 * DFL bits per FRAME_SYM symbols, times 188/187 for the sync
		 * byte the receiver puts back. */
		return (uint32_t)((double)cfg->dfl * UP_BYTES / UP_PAYLOAD /
				  FRAME_SYM * 1e6);
	default:
		return 0;
	}
}

uchar *_dvbs2arm_process_packet(uchar *packet188)
{
	int dfbytes = cfg->dfl / 8;
	uchar up[UP_BYTES];
	int n = 0;
	uint32_t *out = 0;

	/* Sync byte is replaced by the CRC-8 of the previous packet's payload. */
	up[0] = have_prev ? crc8(prev_payload, UP_PAYLOAD) : 0;
	memcpy(up + 1, packet188 + 1, UP_PAYLOAD);
	memcpy(prev_payload, packet188 + 1, UP_PAYLOAD);
	have_prev = 1;

	while (n < UP_BYTES) {
		int space = dfbytes - df_fill;
		int take = UP_BYTES - n;

		if (take > space)
			take = space;
		memcpy(datafield + df_fill, up + n, (size_t)take);
		df_fill += take;
		n += take;
		if (df_fill == dfbytes)
			out = emit_frame();
	}
	return (uchar *)out;
}

void _reverse_byte_order(void)
{
}
