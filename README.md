![rpitx banner](/doc/rpitxlogo.png)
# About rpitx
**rpitx** is a general radio frequency transmitter for Raspberry Pi which doesn't require any other hardware unless filter to avoid intererence. It can handle frequencies from 5 KHz up to 1500 MHz.

Rpitx is a software made for educational on RF system. It has not been tested for compliance with regulations governing transmission of radio signals. You are responsible for using your Raspberry Pi legally.

A forum is available : https://groups.io/g/rpitx

_Created by Evariste Courjaud F5OEO. See Licence for using it.

# Installation

Assuming a Raspbian Lite installation (raspios-bookworm) : [https://www.raspberrypi.org/downloads/raspbian/](https://www.raspberrypi.com/software/operating-systems/#raspberry-pi-os-32-bit)

Be sure to have git package installed :
```sh
sudo apt-get update
sudo apt-get install git
```
You can now clone the repository. A script (install.sh) is there for easy installation. You could inspect it and make steps manualy in case of any doubt. You can note that /boot/config.txt should be prompt to be modified during the installation. If it is not accepted, **rpitx** will be unstable.  

This fork keeps the DSP/transmitter dependencies (csdr, librpitx,
ft8_lib) as git submodules, so clone with --recursive:

```sh
git clone --recursive git@github.com:specture724/rpitx.git
cd rpitx
./install.sh
```
Make a reboot in order to use **rpitx** in a stable state.
That's it !
```sh
sudo reboot
```

# Hardware
![bpf](/doc/bpf-warning.png)

| Raspberry Model      | Status  |
| ---------------------|:-------:|
| Pizero|OK|
| PizeroW|OK|
| PiA+|OK|
| PiB|Partial|
| PiB+|OK|
| P2B|OK|
| Pi3B|OK|
| Pi3B+|OK|
| Pi4|In beta mode|
| Pi5|OK (RP1 backends, see below)|

On the Raspberry Pi 5 the output pin depends on the frequency - see the
Pi 5 section below - but on every other model, plug a wire on GPIO 4,
means Pin 7 of the GPIO header ([header P1](http://elinux.org/RPi_Low-level_peripherals#General_Purpose_Input.2FOutput_.28GPIO.29)). This acts as the antenna. The optimal length of the wire depends the frequency you want to transmit on, but it works with a few centimeters for local testing.

# Raspberry Pi 5 (RP1) support

The Raspberry Pi 5 uses the RP1 south bridge, which drops the BCM2835
PLL/PWM/PCM DMA paths the original rpitx relies on. This fork adds RP1
backends so the most useful modes transmit again.

Run the tools with `sudo` (they need /dev/pio0 and write the PCIe ASPM
policy so GPIO reads stay fast). A reboot after install is recommended.

## What works on Pi 5

Everything except DVB-S2. The tools do not choose a backend themselves - a
factory in librpitx picks one from the requested carrier:

| Backend | Used for | Carrier range | Timing |
|---|---|---|---|
| RP1 PIO + its DMA | FM/AM/IQ sample streams | up to 25 MHz | sample-exact, DMA paced |
| `pll_video` + CPU | everything above that, and all OOK/FSK bursts | up to 1.6 GHz | CPU paced, measured 1.4 ppm over a 12.6 s FT8 frame |
| PIO bit-serial + NCO | DVB-S (PSK) | 25 MHz, UHF on a harmonic | DMA paced, carrier exact to 0.02 Hz |

| Tool | Status | Notes |
|---|---|---|
| `tune` | works | two bands, see below |
| `rpitx -m RF/RFA/IQ/IQFLOAT` | works | PIO under 25 MHz, pll_video above |
| `piofm`, `pio_fsk` | works | standalone PIO tools, HF only |
| `morse`, `sendook` | works | carrier keyed by the pad's OD bit |
| `pocsag`, `pift8`, `corel8` | works | FSK by rewriting `fbdiv_frac` |
| `piopera` | works | OOK envelope |
| `pisstv`, `pirtty`, `pifsq`, `pichirp`, `foxhunt` | works | FM, runs of equal samples are coalesced |
| `freedv`, `pifmrds` | works | high sample rates, still paced correctly |
| `sendiq`, `spectrumpaint` | works | polar (frequency + keyed envelope) |
| `dvbrf -m dvbs` | works | QPSK by phase-accumulator synthesis on the PIO |
| `dvbrf -m dvbs2` | **not ported** | its encoder is 32-bit ARM assembly with no C equivalent |

Two things behave differently from the BCM2835 original:

- **Amplitude is one bit.** The BCM path varies the pad drive strength per
  sample to get 8 amplitude levels. Neither RP1 path can do that, so AM is
  an on/off envelope and SSB is polar FM plus keying. Voice is intelligible
  but distorted.
- **A tool killed with SIGKILL cannot clean up**, so it may leave the
  carrier running and `pll_video` borrowed. Ctrl-C is fine.

### DVB-S

`dvbsenco8.s` (energy dispersal, RS(204,188), convolutional interleaver) was
rewritten in C and checked bit-identical against the original assembly over
2000 packets of varied payloads, so the outer coding is unchanged - the
assembly only existed to hit 15 us/packet on a 700 MHz Pi 1.

The modulator streams the carrier itself rather than a sample stream. The
BCM2835 path clocks a serialiser at carrier*phases and rotates a repeating
pattern to step the phase; on the RP1 the PIO clock divider has only 8
fractional bits, and that error is multiplied by the harmonic, so instead the
serialiser runs at an exact integer division of the 200 MHz PIO clock and the
carrier comes from a 32-bit phase accumulator. Frequency is then exact to
~0.02 Hz and phase steps stay exact for any number of phases.

It is limited by DMA throughput, not the PIO: measured 5.89 Mword/s
(188 Mbit/s) with 32 KB buffers, so the serial rate is held at 100 Mbit/s and
the carrier ceiling is 25 MHz. UHF is reached on an odd harmonic, as it is on
the BCM2835. A 20 second run at 434 MHz / 250 kSym/s showed no underruns.

DVB-S2 is a different matter: its encoder (BCH + LDPC for every code rate) is
2700 lines of 32-bit ARM assembly with no C equivalent in the tree, so on
64-bit builds `-m dvbs2` reports that it is unavailable.

Anything still using the BCM2835 DMA path now stops with a clear message
instead of writing into unrelated RP1 registers.

## Carrier frequency on the Pi 5: two bands, two pins

`tune` picks the synthesiser from the frequency you ask for, and **the
output pin changes with it**:

| Frequency | Synthesised by | Output pin |
|---|---|---|
| exact divide of 200 MHz (100, 50, 25, 10 MHz ...) | GP0 divider off `pll_sys` | **GPIO4** (header pin 7) |
| everything else, up to 1.6 GHz | `pll_video` PLL -> GP2 | **GPIO6** (header pin 31) |

GP0 is a clock *divider*, not a PLL: asking it for a non-integer ratio
makes it dither between two divisors and the output becomes a comb of
spurs instead of a carrier (200/1.3333 for 150 MHz was a bad case). So
GP0 is only used when `pll_sys` divides exactly; anything else goes to
`pll_video`, which is a real PLL and can be followed by a large integer
divider.

The high band works the way the BCM2835 port does: a fractional-N PLL is
retuned to the carrier. RP1's `pll_video` is idle unless a DPI/DSI panel
is attached, so rpitx borrows it and hands it back on exit. Its 24-bit
feedback fraction gives ~1-3 Hz resolution anywhere in the band (the
BCM2835 has 20 bits). Measured with RP1's on-chip frequency counter, the
VCO is exact from 600 MHz to 2 GHz; 1.6 GHz is used as a conservative
ceiling.

### Spectral purity: prefer an integer-N frequency

When the VCO can be an exact multiple of the 50 MHz crystal *and* an
exact integer multiple of your carrier, the PLL runs integer-N with its
delta-sigma modulator off and the carrier is clean. Otherwise it runs
fractional-N and you will see DSM spurs either side of the carrier.
`tune` says which one it picked:

```
(fbdiv 30+0/2^24, integer-N: no DSM spurs)         <- clean
(fbdiv 26+671089/2^24, fractional-N: expect ...)   <- spurs
```

Integer-N needs `carrier x R = 50 MHz x n` for some integer `R` that
factors into the dividers. Useful clean spots: 145, 150, 200, 250, 300,
400, 425, 433.333333, 450, 500, 1500 MHz. 434.000 MHz cannot be
integer-N from a 50 MHz reference, so it will always have spurs - when
that happens rpitx prints the nearest frequency that can be done
integer-N.

### Frequency accuracy (`-p`)

`-p <ppm>` now works on the Pi 5 and corrects the RP1 crystal in both
bands. It is off unless you pass it: rpitx also reads an NTP-derived ppm,
but on the Pi 5 that disciplines the *system* clock (a BCM2712
oscillator) and says nothing about RP1's 50 MHz crystal, so it is not
applied to the synthesiser.

Calibrate against a signal of known frequency, not against an
uncalibrated RTL-SDR - a dongle without a TCXO is routinely 50-100 ppm
off, which looks like 30-100 kHz of "error" at UHF.

Two caveats:

- **Borrowing `pll_video` disables DPI/DSI display output** while a
  transmit tool is running. It is restored on exit - but a tool killed
  with SIGKILL cannot restore it, and `tune -k` deliberately leaves the
  carrier (and the PLL) running.
- Above 200 MHz you must move your wire from GPIO4 to **GPIO6**. rpitx
  prints a reminder when it switches bands.

The high band is not carrier-only: the modulated modes reach it too, by
keying or retuning `pll_video` from the CPU instead of streaming samples
through the PIO. Only the standalone `piofm`/`pio_fsk` tools are PIO-only
and therefore still capped at ~25 MHz.

## Frequency constraints of the PIO backends

The PIO waveform is a variable-period square wave: carrier =
`PIO_CLK / (2 * (P+3))` with `P >= 1`, so the ceiling is ~25 MHz and the
period is quantized in integer half-cycles (fine below ~1 MHz, coarse at
HF). The sample rate must stay at or below the carrier (`X >= 2`).

## Examples

```sh
# VFO: 10 MHz carrier (low band, wire on GPIO4)
sudo ./tune -f 10000000

# VFO: 434 MHz carrier (high band, wire on GPIO6)
sudo ./tune -f 434000000

# FM tone: 20 kHz carrier, +/-5 kHz deviation, 1 kHz tone, 2 s
sudo ./piofm -f 20000 -d 5000 -r 8000 -t 1000 -n 2

# rpitx FM from an RF-format samplerf file (frequency deviation samples)
sudo ./rpitx -i file.samplerf -m RF -f 20000 -s 8000

# rpitx AM from an RF-format samplerf file (amplitude samples)
sudo ./rpitx -i file.samplerf -m RFA -f 100000 -s 8000
```

Wire: GPIO4 (pin 7 of the 40-pin header) with a short wire antenna; for
SDR testing add an attenuator before the receiver input. The PIO square
wave has strong odd harmonics, so a 10 MHz carrier is receivable at
50/70/90 MHz - `pi5_sdr_test.sh` automates this.

# How to use it
![easymenu](/doc/easymenu.png)
## Easytest
**easytest** is the easiest way to start and see some demonstration. All transmission are made on free ISM band (434MHZ).
To launch it, go to rpitx folder and launch easytest.sh :
```sh
cd rpitx
./easytest.sh
```
Choose your choice with arrows and enter to start it.**Don't forget, some test are made in loop, you have to press CTRL^C to exit and back to menu.**

Easy way to monitor what you are doing is by using a SDR software and a SDR receiver like a rtl-sdr one and set the frequency to 434MHZ.

### Carrier ### 
![Carrier](/doc/Tunerpitx.png)
A simple carrier generated at 434MHZ. 

### Chirp ### 
![Chirp](/doc/chirprpitx.png)
A carrier which move around 434MHZ.

### Spectrum ###
![Spectrum](/doc/spectrumrpitx.png)
A picture is displayed on the waterfall on your SDR. Note that you should make some tweaks in order to obtain contrast and correct size depending on your reception and SDR software you use.

### RfMyFace ###
![Rfmyface](/doc/rfmyface.png)
Spectrum painting of your face using the raspicam for fun !

### FM with RDS ###
![FMRDS](/doc/fmrds.png)
Broadcast FM with RDS. You should receive it with your SDR. This is the modulation that you should hear on your classical FM Radio receiver, but at this time, the frequency is too high.

### Single Side Band modulation (SSB) ###
![SSB](/doc/ssbrpitx.png)
This is the classical Hamradio analog voice modulation. Use your SDR in USB mode.

### Slow Scan Television (SSTV) ###
![SSTV](/doc/sstvrpitx.JPG)
This is a picture transmission mode using audio modulation (USB mode). You need an extra software to decode and display it (qsstv,msstv...). This demo uses the Martin1 mode of sstv.


### Pocsag (pager mode) ###
![pocsag](/doc/pocsagrpitx.JPG)
This is a mode used by pagers. You need an extra software to decode. Set your SDR in NBFM mode.

### Freedv (digital voice) ###
![freedv](/doc/freedvrpitx.JPG)
This is state of the art opensource digital modulation. You need Freedv for demodulation.

### Opera (Beacon) ###
![opera](/doc/operarpitx.JPG)
This a beacon mode which sound like Morse. You need opera in mode 0.5 to decode.

## Rpitx and low cost RTL-SDR dongle ##
![rtlmenu](/doc/rlsdrmenu.png)

**rtlmenu** allows to use rtl-sdr receiver dongle and **rpitx** together. This combine receiver and transmission for experimenting. 
To launch it, go to rpitx folder and launch rtlmenu.sh :
```sh
./rtlmenu.sh
```
You have first to set receiver frequency and gain of rtl-sdr. Warning about gain, you should ensure that you have enough gain to receive the signal but not to strong which could saturate it and will not be usefull by **rpitx**.

Choose your choice with arrows and enter to start it.**Don't forget, some test are made in loop, you have to press CTRL^C to exit and back to menu.**


### Record and play ###
![replay](/doc/replay.png)

A typical application, is to replay a signal. Picture above shows a replay of a signal from a RF remote switch.
So first, record few seconds of signal, CTRL^C for stop recording. Then replay it with play.

### Transponder ###
![fmtransponder](/doc/fmtransponder.png)
We can also live transmitting a received band frequency. Here the input frequency is a FM broadcast station which is retransmit on 434MHZ.

### Relay with transmodulation ###
We assume that input frequency is tuned on FM station. It is demodulated and modulate to SSB on 434MHZ. SSB is not HiFi, so prefere to choose a talk radio, music sounds like bit weird !


# To continue
**rpitx** is a generic RF transmitter. There is a lot of modulation to do with it and also documentation to make all that easy to contribute. This will be the next step ! Feel free to inspect scripts, change parameters (frequencies, audio input, pictures...). 

# Credits
All rights of the original authors reserved.
I try to include all licences and authors in sourcecode. Need to write all references in this section.  
