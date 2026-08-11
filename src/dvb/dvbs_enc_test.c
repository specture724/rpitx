/*
 * Algorithm-level checks for the C DVB-S outer coder. These prove properties
 * rather than comparing against another implementation (that is done
 * separately against the original ARM32 assembly):
 *
 *  - RS: a codeword must evaluate to zero at the 16 generator roots a^0..a^15.
 *  - PRBS: maximal length, and the payload must equal an independently
 *    written generator.
 *  - interleaver: a matching deinterleaver must recover the stream. In a
 *    Forney interleaver branch j shifts once per commutator revolution, so
 *    its delay is j*M*I stream bytes; end to end that is (I-1)*M*I = 2244
 *    bytes, exactly 11 packets - which is why the assembly's header says the
 *    first 11 output packets are incomplete.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

typedef unsigned char uchar;
extern void dvbsenco_init(void);
extern uchar *dvbsenco(uchar *packetin);
extern void energy(uchar *in, uchar *out);

/* independent GF(256), same primitive polynomial */
static uchar e[512], l[256];
static void gfinit(void){unsigned x=1;for(int i=0;i<255;i++){e[i]=x;l[x]=i;x<<=1;if(x&0x100)x^=0x11D;}for(int i=255;i<512;i++)e[i]=e[i-255];}
static uchar mul(uchar a,uchar b){return (!a||!b)?0:e[l[a]+l[b]];}

static int fails = 0;
static void ok(const char *what, int cond)
{
	printf("  %-54s %s\n", what, cond ? "PASS" : "FAIL");
	if (!cond) fails++;
}

/* independent PRBS: stage k at bit (15-k), out = s14^s15 fed back to s1 */
static uint16_t r;
static void prbs_load(void){ r = 0x4A80; }
static uchar prbs_b(void){
	uchar o=0;
	for(int i=0;i<8;i++){uint16_t b=(uint16_t)(((r>>1)^r)&1);o=(uchar)((o<<1)|b);r=(uint16_t)((r>>1)|(b<<14));}
	return o;
}

#define I 12
#define M 17
#define INTER_DELAY ((I-1)*M*I)     /* 2244 bytes = 11 packets */
#define NQ 80

int main(void)
{
	gfinit();
	printf("DVB-S outer coder self-checks\n");

	/* 1. PRBS is maximal length */
	{
		prbs_load();
		uint16_t first = r; long bits = 0;
		do { uint16_t b=(uint16_t)(((r>>1)^r)&1); r=(uint16_t)((r>>1)|(b<<14)); bits++; }
		while (r != first && bits < 100000);
		ok("PRBS register period is 32767 bits (maximal length)", bits == 32767);
	}

	/* 2. randomiser matches an independent generator, zero payload in */
	{
		uchar zero[188], out[188];
		memset(zero, 0, sizeof(zero)); zero[0] = 0x47;
		dvbsenco_init();
		energy(zero, out);
		ok("sync byte inverted to 0xB8 at the start of a group", out[0] == 0xB8);
		prbs_load();
		int match = 1;
		for (int i = 1; i < 188; i++) if (out[i] != prbs_b()) { match = 0; break; }
		ok("randomised payload equals an independent PRBS", match);
	}

	/* 3. full chain: deinterleave, check RS syndromes, derandomise */
	{
		static uchar in[NQ][188];
		static uchar stream[NQ*204], deint[NQ*204];
		static uchar fifo[I][I*M]; static int pos[I];
		int bad_syn = 0, bad_pay = 0, checked = 0;

		dvbsenco_init();
		srandom(999);
		for (int p = 0; p < NQ; p++) {
			in[p][0] = 0x47;
			for (int i = 1; i < 188; i++) in[p][i] = random() & 0xFF;
			memcpy(stream + (long)p*204, dvbsenco(in[p]), 204);
		}

		memset(fifo,0,sizeof(fifo)); memset(pos,0,sizeof(pos));
		int br = 0;
		for (long k = 0; k < (long)NQ*204; k++) {
			int d = (I-1-br) * M;          /* mirror of the interleaver */
			uchar v = stream[k];
			if (d == 0) deint[k] = v;
			else { int q = pos[br]; deint[k] = fifo[br][q]; fifo[br][q] = v;
			       if (++q >= d) q = 0; pos[br] = q; }
			if (++br >= I) br = 0;
		}

		/* codeword for input packet p starts at INTER_DELAY + p*204 */
		prbs_load();
		int gb = 0;
		for (int p = 0; p < NQ; p++) {
			long base = INTER_DELAY + (long)p*204;
			if (base + 204 > (long)NQ*204) break;
			const uchar *cw = deint + base;
			int syn = 0;
			for (int root = 0; root < 16; root++) {
				uchar s2 = 0;
				for (int i = 0; i < 204; i++) s2 = mul(s2, e[root]) ^ cw[i];
				if (s2) syn = 1;
			}
			if (syn) bad_syn++;
			for (int i = 0; i < 188; i++) {
				uchar v = cw[i];
				if (gb == 0) { prbs_load(); v = 0x47; }
				else if ((gb % 188) == 0) { (void)prbs_b(); v = 0x47; }
				else v ^= prbs_b();
				if (++gb >= 8*188) gb = 0;
				if (v != in[p][i]) { bad_pay++; break; }
			}
			checked++;
		}
		printf("  (checked %d packets through the full chain)\n", checked);
		ok("deinterleaved codewords have zero RS syndrome", bad_syn == 0 && checked > 0);
		ok("payload recovered after deinterleave + derandomise", bad_pay == 0 && checked > 0);
	}

	printf("\n%s (%d failure%s)\n", fails ? "FAILURES" : "all checks passed",
	       fails, fails == 1 ? "" : "s");
	return fails ? 1 : 0;
}
