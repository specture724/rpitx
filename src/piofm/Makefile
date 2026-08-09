# Pi 5 PIO-based transmitter tools (use the official rp1-pio /dev/pio0 API).
# Requires the running kernel's headers (linux/pio_rp1.h).

# the rp1-pio UAPI header lives in the common kernel-headers tree
PIOFM_INC := $(patsubst %/linux/pio_rp1.h,%,$(firstword $(wildcard /usr/src/linux-headers-*/include/linux/pio_rp1.h)))
CFLAGS_PIOFM := -O2 -idirafter $(PIOFM_INC)

all: ../piofm ../pio_fsk

../piofm: piofm.c
	$(CC) $(CFLAGS_PIOFM) -o $@ piofm.c -lm

../pio_fsk: pio_fsk.c
	$(CC) $(CFLAGS_PIOFM) -o $@ pio_fsk.c -lm

clean:
	rm -f ../piofm ../pio_fsk
