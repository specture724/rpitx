/*
 * piofm - Pi 5 FM/FSK transmitter using the RP1 PIO + its DMA path.
 *
 * Usage: piofm -f <carrier_Hz> -d <deviation_Hz> -r <sample_rate> [-t tone_Hz]
 *              [-i input.raw] [-v] [-n seconds]
 *
 * Reads mono 16-bit little-endian audio (or generates a tone), converts each
 * sample to a (P, X) period word pair, and feeds them to the PIO state
 * machine through the driver-managed DMA. The PIO generates the FM'd
 * square-wave carrier on GPIO4.
 */
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <math.h>
typedef unsigned int uint;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef uint64_t dma_addr_t;
typedef _Bool bool;
#ifndef true
#define true 1
#define false 0
#endif
#define WARN_ON(x) ((void)0)
#include <linux/pio_rp1.h>

#define OUT_GPIO 4
#define PIO_CLK  200000000.0

static uint16_t prog[RP1_PIO_INSTRUCTION_COUNT];

static void build_program(void)
{
	prog[0]  = pio_encode_pull(false, true);
	prog[1]  = pio_encode_in(pio_osr, 32);
	prog[2]  = pio_encode_mov(pio_y, pio_osr);
	prog[3]  = pio_encode_pull(false, true);
	prog[4]  = pio_encode_mov(pio_x, pio_osr);
	prog[5]  = pio_encode_set(pio_pins, 1);
	prog[6]  = pio_encode_jmp_y_dec(6);
	prog[7]  = pio_encode_mov(pio_y, pio_isr);
	prog[8]  = pio_encode_set(pio_pins, 0);
	prog[9]  = pio_encode_jmp_x_dec(11);
	prog[10] = pio_encode_jmp(0);
	prog[11] = pio_encode_jmp_y_dec(11);
	prog[12] = pio_encode_mov(pio_y, pio_isr);
	prog[13] = pio_encode_set(pio_pins, 1);
	prog[14] = pio_encode_jmp_x_dec(6);
	prog[15] = pio_encode_jmp(0);
}

static int pio_setup(int fd, uint16_t *off)
{
	struct rp1_pio_add_program_args ap;
	memset(&ap, 0, sizeof(ap));
	ap.num_instrs = 16;
	ap.origin = RP1_PIO_ORIGIN_ANY;
	memcpy(ap.instrs, prog, 16 * sizeof(uint16_t));
	int r = ioctl(fd, PIO_IOC_ADD_PROGRAM, &ap);
	if (r < 0) return -1;
	*off = (uint16_t)r;
	struct rp1_pio_sm_claim_args cl = { .mask = 1 };
	ioctl(fd, PIO_IOC_SM_CLAIM, &cl);
	struct rp1_gpio_init_args gi = { .gpio = OUT_GPIO };
	ioctl(fd, PIO_IOC_GPIO_INIT, &gi);
	struct rp1_gpio_set_function_args gf = { .gpio = OUT_GPIO, .fn = GPIO_FUNC_PIO };
	ioctl(fd, PIO_IOC_GPIO_SET_FUNCTION, &gf);
	pio_sm_config c = pio_get_default_sm_config();
	sm_config_set_set_pins(&c, OUT_GPIO, 1);
	struct rp1_pio_sm_init_args si = { .sm = 0, .initial_pc = *off, .config = c };
	ioctl(fd, PIO_IOC_SM_INIT, &si);
	struct rp1_pio_sm_set_pindirs_args pd = { .sm = 0, .dirs = 1u << OUT_GPIO,
						  .mask = 1u << OUT_GPIO };
	ioctl(fd, PIO_IOC_SM_SET_PINDIRS, &pd);
	struct rp1_pio_sm_config_xfer32_args cx = { .sm = 0, .dir = PIO_DIR_TO_SM,
						    .buf_size = 1024, .buf_count = 4 };
	return ioctl(fd, PIO_IOC_SM_CONFIG_XFER32, &cx);
}

int main(int argc, char **argv)
{
	double carrier = 1e6, dev = 50e3, rate = 192000, tone = 1000;
	const char *infile = NULL;
	int verify = 0, nsec = 1;
	int opt;
	while ((opt = getopt(argc, argv, "f:d:r:t:i:vn:")) != -1) {
		switch (opt) {
		case 'f': carrier = atof(optarg); break;
		case 'd': dev = atof(optarg); break;
		case 'r': rate = atof(optarg); break;
		case 't': tone = atof(optarg); break;
		case 'i': infile = optarg; break;
		case 'v': verify = 1; break;
		case 'n': nsec = atoi(optarg); break;
		default: return 1;
		}
	}

	/* read audio (mono 16-bit LE) or generate a tone */
	int nsamp = (int)(rate * nsec);
	int16_t *audio = malloc(nsamp * 2);
	if (infile) {
		int f = open(infile, O_RDONLY);
		if (f < 0) { perror("open input"); return 1; }
		int got = read(f, audio, nsamp * 2);
		nsamp = got / 2;
		close(f);
	} else {
		for (int i = 0; i < nsamp; i++)
			audio[i] = (int16_t)(30000.0 * sin(2 * M_PI * tone * i / rate));
	}

	/* DSP: audio sample -> (P, X) */
	uint32_t *samples = malloc(nsamp * 2 * 4);
	for (int i = 0; i < nsamp; i++) {
		double a = audio[i] / 32768.0;
		double fi = carrier + dev * a;          /* instantaneous freq */
		uint32_t P = (uint32_t)(PIO_CLK / (2.0 * fi)) - 3;
		if (P < 1) P = 1;
		uint32_t X = (uint32_t)(PIO_CLK / (rate * ((double)P + 3)));
		if (X < 1) X = 1;
		samples[i * 2] = P;
		samples[i * 2 + 1] = X;
	}
	printf("piofm: carrier=%.0f dev=%.0f rate=%.0f samples=%d\n",
	       carrier, dev, rate, nsamp);

	int fd = open("/dev/pio0", O_RDWR);
	if (fd < 0) { perror("open /dev/pio0"); return 1; }
	uint16_t off;
	if (pio_setup(fd, &off) < 0) { perror("pio_setup"); return 1; }

	struct rp1_pio_sm_set_enabled_args en = { .mask = 1, .enable = 1 };
	ioctl(fd, PIO_IOC_SM_SET_ENABLED, &en);
	pid_t pid = fork();
	if (pid == 0) {
		struct rp1_pio_sm_xfer_data32_args xd = { .sm = 0, .dir = PIO_DIR_TO_SM,
							 .data_bytes = nsamp * 8,
							 .data = samples };
		int xr = ioctl(fd, PIO_IOC_SM_XFER_DATA32, &xd);
		_exit(xr < 0 ? 1 : 0);
	}

	if (verify) {
		/* count transitions per window and print the implied frequency */
		int mfd = open("/dev/mem", O_RDWR | O_SYNC);
		volatile uint32_t *g = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
					    MAP_SHARED, mfd, 0x1f000d0000);
		if (g == MAP_FAILED) { perror("mmap"); return 1; }
		double win = 1000.0 / rate;  /* 1000-sample windows */
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		uint64_t t0 = ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
		uint64_t ws = 0;
		unsigned long trans = 0;
		int prev = -1, wi = 0;
		while (1) {
			int level = (g[0x20 / 4] >> 23) & 1;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			uint64_t us = ts.tv_sec * 1000000 + ts.tv_nsec / 1000 - t0;
			if (prev >= 0 && level != prev) trans++;
			prev = level;
			if (us - ws >= 1000 * win) {
				double freq = trans / (2.0 * win);
				printf("win %3d: freq=%.0f Hz\n", wi, freq);
				trans = 0; ws = us; wi++;
				if (wi >= nsamp / 1000) break;
			}
		}
	}
	waitpid(pid, NULL, 0);
	en.enable = 0;
	ioctl(fd, PIO_IOC_SM_SET_ENABLED, &en);
	printf("done\n");
	return 0;
}
