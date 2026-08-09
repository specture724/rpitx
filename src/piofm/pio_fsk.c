#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
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
#define GPIO_STAT 0x1f000d020ULL   /* pin 4 STATUS register */

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

int main(void)
{
	build_program();
	int fd = open("/dev/pio0", O_RDWR);
	if (fd < 0) { perror("open /dev/pio0"); return 1; }

	struct rp1_pio_add_program_args ap;
	memset(&ap, 0, sizeof(ap));
	ap.num_instrs = 16;
	ap.origin = RP1_PIO_ORIGIN_ANY;
	memcpy(ap.instrs, prog, 16 * sizeof(uint16_t));
	int r = ioctl(fd, PIO_IOC_ADD_PROGRAM, &ap);
	if (r < 0) { perror("ADD_PROGRAM"); return 1; }
	uint16_t off = (uint16_t)r;

	struct rp1_pio_sm_claim_args cl = { .mask = 1 };
	ioctl(fd, PIO_IOC_SM_CLAIM, &cl);
	struct rp1_gpio_init_args gi = { .gpio = OUT_GPIO };
	ioctl(fd, PIO_IOC_GPIO_INIT, &gi);
	struct rp1_gpio_set_function_args gf = { .gpio = OUT_GPIO, .fn = GPIO_FUNC_PIO };
	ioctl(fd, PIO_IOC_GPIO_SET_FUNCTION, &gf);
	pio_sm_config c = pio_get_default_sm_config();
	sm_config_set_set_pins(&c, OUT_GPIO, 1);
	struct rp1_pio_sm_init_args si = { .sm = 0, .initial_pc = off, .config = c };
	ioctl(fd, PIO_IOC_SM_INIT, &si);
	struct rp1_pio_sm_set_pindirs_args pd = { .sm = 0, .dirs = 1u << OUT_GPIO,
						  .mask = 1u << OUT_GPIO };
	ioctl(fd, PIO_IOC_SM_SET_PINDIRS, &pd);

	struct rp1_pio_sm_config_xfer32_args cx = { .sm = 0, .dir = PIO_DIR_TO_SM,
						    .buf_size = 1024, .buf_count = 4 };
	if (ioctl(fd, PIO_IOC_SM_CONFIG_XFER32, &cx) < 0) {
		perror("CONFIG_XFER"); return 1;
	}

	/* FSK: 6 symbols, 100 ms each, alternating 10 kHz / 20 kHz */
	const double freqs[] = { 10000, 20000, 10000, 20000, 10000, 20000 };
	const double T_sym = 0.1;
	int nsym = 6;
	uint32_t samples[6 * 2];
	for (int i = 0; i < nsym; i++) {
		uint32_t P = (uint32_t)(PIO_CLK / (2.0 * freqs[i])) - 3;
		uint32_t X = (uint32_t)(PIO_CLK * T_sym / ((double)P + 3));
		samples[i * 2] = P;
		samples[i * 2 + 1] = X;
		printf("sym %d: f=%.0f P=%u X=%u (carrier=%.0f)\n", i, freqs[i],
		       P, X, PIO_CLK / (2.0 * ((double)P + 3)));
	}

	struct rp1_pio_sm_set_enabled_args en = { .mask = 1, .enable = 1 };
	ioctl(fd, PIO_IOC_SM_SET_ENABLED, &en);

	pid_t pid = fork();
	if (pid == 0) {
		struct rp1_pio_sm_xfer_data32_args xd = { .sm = 0, .dir = PIO_DIR_TO_SM,
							 .data_bytes = sizeof(samples),
							 .data = samples };
		int xr = ioctl(fd, PIO_IOC_SM_XFER_DATA32, &xd);
		_exit(xr < 0 ? 1 : 0);
	}

	/* verify: count transitions per 100 ms window, compare with expected */
	int mfd = open("/dev/mem", O_RDWR | O_SYNC);
	volatile uint32_t *g = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
				    MAP_SHARED, mfd, 0x1f000d0000);
	if (g == MAP_FAILED) { perror("mmap gpio"); return 1; }
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t t0 = ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
	int prev = -1;
	uint64_t window_start = 0;
	unsigned long trans = 0;
	int wi = 0;
	while (1) {
		int level = (g[0x20 / 4] >> 23) & 1;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		uint64_t us = ts.tv_sec * 1000000 + ts.tv_nsec / 1000 - t0;
		if (prev >= 0 && level != prev) trans++;
		prev = level;
		if (us - window_start >= 100000) {
			printf("window %d: transitions=%lu expected=%.0f\n", wi,
			       trans, freqs[wi] * 2 * T_sym);
			trans = 0;
			window_start = us;
			wi++;
			if (wi >= nsym) break;
		}
	}
	waitpid(pid, NULL, 0);
	en.enable = 0;
	ioctl(fd, PIO_IOC_SM_SET_ENABLED, &en);
	printf("done\n");
	return 0;
}
