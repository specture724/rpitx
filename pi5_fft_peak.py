#!/usr/bin/env python3
"""Find the strongest peak in an rtl_sdr IQ capture and report its frequency.

usage: pi5_fft_peak.py iq.bin center_hz samplerate_hz [min_hz max_hz]
  center/samplerate are what you passed to rtl_sdr (-f / -s).
"""
import sys
import numpy as np

def main():
    fn, center, rate = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
    lo = float(sys.argv[4]) if len(sys.argv) > 4 else center - rate/2
    hi = float(sys.argv[5]) if len(sys.argv) > 5 else center + rate/2
    raw = np.fromfile(fn, dtype=np.int16)
    if raw.size < 4096:
        print("capture too short (%d samples) - is the SDR connected and "
              "is something transmitting?" % raw.size)
        return 1
    n = (len(raw) // 2) * 2
    iq = raw[:n:2].astype(np.float64) + 1j * raw[1:n:2].astype(np.float64)
    # average magnitude spectra over chunks to reduce noise
    nfft = 16384
    nchunk = max(1, min(32, len(iq) // nfft))
    spec = np.zeros(nfft)
    for k in range(nchunk):
        seg = iq[k*nfft:(k+1)*nfft]
        w = np.hanning(len(seg))
        spec += np.abs(np.fft.fftshift(np.fft.fft(seg * w))) ** 2
    spec /= nchunk
    freqs = np.fft.fftshift(np.fft.fftfreq(nfft, 1.0/rate)) + center
    mask = (freqs >= lo) & (freqs <= hi)
    if not mask.any():
        print("no frequency in range"); return 1
    i = np.argmax(spec * mask)
    # parabolic interpolation for sub-bin precision
    if 0 < i < nfft-1 and spec[i-1] < spec[i] > spec[i+1]:
        d = 0.5 * (np.log(spec[i-1]) - np.log(spec[i+1])) / \
            (np.log(spec[i-1]) - 2*np.log(spec[i]) + np.log(spec[i+1]))
        df = np.fft.fftshift(np.fft.fftfreq(nfft, 1.0/rate))[1] - \
             np.fft.fftshift(np.fft.fftfreq(nfft, 1.0/rate))[0]
        pk = freqs[i] + d * df
    else:
        pk = freqs[i]
    print("peak: %.1f Hz  (bin %.1f Hz, snr %.1f dB)" %
          (pk, abs(df) if 'df' in dir() else rate/nfft,
           10*np.log10(spec[i] / np.median(spec[mask]))))
    return 0

if __name__ == "__main__":
    sys.exit(main())
