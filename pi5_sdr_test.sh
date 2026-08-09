#!/bin/sh
# Verify the Raspberry Pi 5 PIO FM transmitter (rpitx -m RF) with an SDR.
#
# usage: ./pi5_sdr_test.sh carrier [deviation] [tone] [duration]
#   carrier   base carrier in Hz (default 10000000 -> 10 MHz)
#             the SDR cannot tune below ~24 MHz, so the square-wave ODD
#             harmonics are used: n*carrier for odd n. The script prints
#             the best harmonic inside the RTL-SDR range.
#   deviation FM deviation in Hz (default 0 -> unmodulated carrier,
#             best for the frequency-accuracy check)
#   tone      audio tone in Hz (default 1000)
#   duration  seconds (default 5)
#
# Wiring: GPIO4 (pin 7, the PIO output) -> 30-40 dB attenuator -> SDR
# antenna input; connect Pi GND to the SDR ground. Do NOT drive the SDR
# directly with the 3.3 V square wave - the broadband harmonics will
# overload its frontend.
#
# Requires: python3-numpy, librtlsdr tools (rtl_sdr / rtl_fm).

CARRIER=${1:-10000000}
DEV=${2:-0}
TONE=${3:-1000}
DUR=${4:-5}
RATE=8000

# find the lowest ODD harmonic in the RTL-SDR sweet spot (>= 50 MHz)
HARM=""
HN=0
N=3
while [ $((N * CARRIER)) -le 1700000000 ]; do
	H=$((N * CARRIER))
	if [ "$H" -ge 50000000 ]; then
		HARM=$H
		HN=$N
		break
	fi
	N=$((N + 2))
done
if [ -z "$HARM" ]; then
	echo "no odd harmonic of $CARRIER Hz lands in 50 MHz..1.7 GHz"
	exit 1
fi
echo "base carrier: $CARRIER Hz (PIO square wave)"
echo "SDR target:   $HARM Hz ($HN-th harmonic; FM deviation x$HN)"

python3 - "$CARRIER" "$DEV" "$TONE" "$DUR" "$RATE" <<'EOF'
import struct, math, sys
carrier, dev, tone, dur, rate = map(float, sys.argv[1:6])
n = int(rate * dur)
with open('/tmp/pi5fm.samplerf', 'wb') as f:
	for i in range(n):
		d = dev * math.sin(2 * math.pi * tone * i / rate)
		f.write(struct.pack('<dII', d, 1, 0))
print("wrote %d samples (%.1f s at %.0f Hz)" % (n, dur, rate))
EOF

echo
echo ">>> starting rpitx -m RF (Ctrl-C to stop) in background..."
sudo ./rpitx -i /tmp/pi5fm.samplerf -m RF -f "$CARRIER" -s "$RATE" &
RPID=$!
cleanup() { sudo kill $RPID 2>/dev/null; exit 0; }
trap cleanup INT TERM

sleep 1
if [ "$DEV" = "0" ]; then
	echo ">>> capturing 2 s of IQ at $HARM Hz..."
	timeout 3 rtl_sdr -f "$HARM" -s 1000000 /tmp/pi5fm_iq.bin 2>/dev/null
	python3 ./pi5_fft_peak.py /tmp/pi5fm_iq.bin "$HARM" 1000000 \
		$((HARM - 20000)) $((HARM + 20000))
	echo ">>> expect the peak at $HARM Hz (square-wave harmonic of $CARRIER)"
elif [ "$CARRIER" -lt 14400000 ]; then
	echo ">>> FM audio via RTL direct sampling (Q input) at $CARRIER Hz..."
	echo ">>> deviation $DEV Hz; needs an rtl-sdr v3 (or direct-sampling mod)"
	rtl_fm -f "$CARRIER" -M fm -s 48k -A fast -E direct -r 48k - \
		| aplay -r 48000 -f S16_LE -t raw -c 1
else
	echo ">>> carrier $CARRIER Hz >= 14.4 MHz: the integer-period PIO "
	echo ">>> program cannot represent a $DEV Hz deviation there (step"
	echo ">>> size is ~1 MHz). Re-run with DEV=0 for the frequency check,"
	echo ">>> or with a carrier <= 1 MHz for real FM audio."
fi

cleanup
