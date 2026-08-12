# Porting DVB-S2 off the ARM32 assembly

`dvbs2arm_1v30.s` (G4EWJ, v1.30) is the only remaining 32-bit ARM assembly in
the tree, so `-m dvbs2` is unavailable on 64-bit builds. This is the working
specification for replacing it with C, derived from the assembly itself rather
than guessed from the standard.

## Why this is tractable

It is not a full DVB-S2 encoder. Its own header limits it to:

* **short frames only** (N_ldpc = 16200)
* **QPSK only**
* **FEC 1/4 and 3/4 only**
* rolloff 0.20 / 0.25 / 0.35 (only affects one BBHEADER field)

So only two LDPC tables are needed, not the whole standard's set.

Bit budget for FEC 3/4 short, confirmed against the running encoder:

| field | bits | bytes | offset in `frame` |
|---|---|---|---|
| BBHEADER | 80 | 10 | 0 |
| DATAFIELD | 11632 | 1454 | 10 |
| BBFRAME | 11712 | 1464 | - |
| BCH parity | 168 | 21 | 1464 |
| BCHFRAME | 11880 | 1485 | - |
| LDPC parity | 4320 | 540 | 1485 |
| FECFRAME | 16200 | 2025 | - |
| PLHEADER | 180 | - | prepended as IQ |
| PLFRAME | 16380 | - | 8190 QPSK symbols |

## The oracle

The Pi 5's Cortex-A76 runs AArch32 at EL0, so the original assembly can be
built with `arm-linux-gnueabihf-gcc -marm` and executed natively. That gives a
byte-exact reference to diff against - the same technique that verified the
DVB-S rewrite.

Two things are needed to assemble it:

* prepend `.arm` and `.syntax divided` (it uses the two-operand shift form,
  e.g. `orr r1,r2,lsr#24`, which unified syntax rejects)
* add `.global` for any internal label you want to inspect

`ldpcs_encode` cannot be driven in isolation: it works through pointers that
`_dvbs2arm_control` sets up, so passing your own buffer does nothing. Instead
make the internal `frame` buffer global and read it after
`_dvbs2arm_process_packet` returns a frame - that exposes the FECFRAME after
scrambling, BCH and LDPC in one go.

## Output format

`_dvbs2arm_process_packet` returns 0 until a frame is ready, then a pointer to
546 words: 273 (I, Q) pairs, each carrying 30 symbols in bits 31..2, MSB first.
90 PLHEADER symbols + 8100 data symbols = 8190.

## Mode adaptation, determined empirically

Descrambling the captured frame (XOR with `bbframe_scramble_table`) and
correlating it against known input packets shows the datafield is a continuous
byte stream where each user packet contributes **188 bytes**:

```
[ CRC-8 of the *previous* UP's 187 payload bytes ][ this UP's 187 payload bytes ]
```

i.e. the sync byte is replaced by the CRC-8 of the preceding packet, computed
with `crc8_table` over payload bytes 1..187. Verified for seven consecutive
packets: the byte before each payload matched `crc8(previous payload)` exactly.

`SYNCD` is the bit offset of the first UP boundary within the datafield.

## BBHEADER, read back from the oracle

For FEC 3/4, rolloff 0.35, TS input:

```
MATYPE1 = 0xf0   TS/GS=3 SIS/MIS=1 CCM/ACM=1 ISSYI=0 NPD=0 RO=0
MATYPE2 = 0x00
UPL     = 1504   (188 bytes)
DFL     = 11632
SYNC    = 0x47
SYNCD   = varies
CRC-8   = over the first 9 header bytes, same crc8_table
```

## Tables

`gen_dvbs2_tables.py` produces `dvbs2_tables.h`. Everything except the LDPC
tables is lifted from the assembly, so the C encoder uses identical data:

* `crc8_table` (256 bytes)
* `bbframe_scramble_table` (507 words)
* `bch_s168_table` (2048 words - 256 entries of 6 used words + 2 padding)
* `symbols_scramble_table3` / `4` (PL scrambler)
* `PL_HEADER_S34_IQ` / `PL_HEADER_S14_IQ` (precomputed PLHEADER as IQ words)

The LDPC tables cannot be taken from the assembly - `ldpc_parameters_s34` fuses
addresses with pointers to specialised unrolled routines. They come from GNU
Radio gr-dtv (`ldpc_tab_1_4S[9][13]`, `ldpc_tab_3_4S[33][13]`, GPL-3.0), which
is licence-compatible. They were cross-checked: the first column of the
assembly's `ldpc_parameters_s34` is `3, 3198, 478, 4207, 1481, ...`, which is
exactly `ldpc_tab_3_4S[0]` after its leading degree count of 12.

## Remaining work

1. Encoder in C: mode adaptation, BBHEADER + CRC-8, BB scramble, BCH (168-bit
   remainder via `bch_s168_table`), LDPC (standard accumulate over the tables
   with the trailing running XOR), QPSK map, PL scramble, I/Q word packing.
2. Diff against the oracle in two stages - the `frame` buffer after LDPC, then
   the final IQ words - so a mismatch localises to a stage.
3. Wire into `dvbrf` in place of `dvbs2_stub.c`.

The LDPC accumulate is the standard one: for information bit `i` in group
`g = i/360` with `m = i%360`, XOR the bit into parity positions
`(tab[g][j] + m*q) mod (N-K)` for each address `j`, then run a final
`p[k] ^= p[k-1]` sweep. `q = (N-K)/360` = 12 for 3/4 short, 36 for 1/4 short.
