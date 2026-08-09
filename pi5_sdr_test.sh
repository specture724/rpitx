#!/bin/sh
# Raspberry Pi 5 PIO FM transmitter -> external SDR verification.
#
# The Pi transmits on GPIO4 (the RP1 PIO output); the SDR lives on a
# separate computer. Run the tx mode here, tune the SDR over there.
#
# usage (on the Pi):
#   ./pi5_sdr_test.sh tx [carrier] [deviation] [tone] [duration]
#     carrier   base carrier Hz, default 10000000 (10 MHz).
#              The PIO square wave has strong ODD harmonics, so a 10 MHz
#              carrier is receivable at 50/70/90 MHz... (RTL-SDR range).
#     deviation FM deviation Hz, default 0 (unmodulated carrier, best
#              for a frequency-accuracy check on the SDR spectrum).
#     tone      audio tone Hz (used when deviation != 0), default 1000.
#     duration  seconds, default 5.
#   ./pi5_sdr_test.sh rx [carrier]
#     Optional: if an SDR happens to be attached to the Pi itself, do
#     the harmonic IQ capture + FFT peak check locally.
#
# Wiring on the Pi: GPIO4 (40-pin header pin 7) -> ~1 m wire antenna.
# A 100-470 ohm series resistor is good practice; Pi GND can hang loose.
# Keep the SDR a few meters away. Low-power, short-range experimentation.

MODE=${1:-tx}
CARRIER=${2:-10000000}
DEV=${3:-0}
TONE=${4:-1000}
DUR=${5:-5}
RATE=8000

# lowest ODD harmonic >= 50 MHz (RTL-SDR sweet spot)
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

if [ "$MODE" = "rx" ]; then
	if [ -z "$HARM" ]; then
		echo "no odd harmonic of $CARRIER Hz in 50 MHz..1.7 GHz"
		exit 1
	fi
	echo "capturing 2 s of IQ at $HARM Hz..."
	timeout 3 rtl_sdr -f "$HARM" -s 1000000 /tmp/pi5fm_iq.bin 2>/tmp/rtlsdr.err
	if [ ! -s /tmp/pi5fm_iq.bin ]; then
		echo "rtl_sdr produced no data:"
		cat /tmp/rtlsdr.err
		exit 1
	fi
	python3 ./pi5_fft_peak.py /tmp/pi5fm_iq.bin "$HARM" 1000000 \
		$((HARM - 20000)) $((HARM + 20000))
	echo ">>> expect the peak at $HARM Hz (square-wave harmonic of $CARRIER)"
	exit 0
fi

echo "Pi transmitter: carrier $CARRIER Hz (square wave on GPIO4)"
if [ -n "$HARM" ]; then
	echo "SDR target:     $HARM Hz ($HN-th harmonic) - tune the computer SDR here"
else
	echo "note: no odd harmonic of $CARRIER lands in 50 MHz..1.7 GHz"
fi

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

echo ">>> transmitting for $DUR s..."
sudo ./rpitx -i /tmp/pi5fm.samplerf -m RF -f "$CARRIER" -s "$RATE"

if [ "$DEV" = "0" ]; then
	echo
	echo "done. On the computer SDR, tune to $HARM Hz and look for a"
	echo "carrier (USB/LSB or CW spectrum view; peak should sit exactly"
	echo "at $HARM Hz). For a 10 MHz base the 5th/7th/9th harmonics land"
	echo "at 50/70/90 MHz."
else
	echo
	echo "done. Real FM audio only works below ~1 MHz (integer half-period"
	echo "steps); an HF-capable SDR on the computer can demodulate it at"
	echo "the base carrier $CARRIER Hz. Above that, use dev=0 for the"
	echo "frequency-accuracy check instead."
fi
