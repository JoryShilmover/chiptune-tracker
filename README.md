# Chiptune Tracker (working name)

A touch-first music tracker for iOS and iPadOS that sounds exactly like the Nintendo DS sound hardware and exports songs as WAV/M4A audio, playable `.nds` ROMs and SSEQ/SDAT files.

Status: Milestone 0 (sound chip core). See [SCOPE.md](SCOPE.md) for decisions, architecture and milestones.

## Layout

| Path | What |
| --- | --- |
| `Core/SPU` | `DSSPU`, the DS sound hardware emulator in portable C++20. Driven only through the real hardware registers. |
| `Tools/spu-render` | Command-line tool that renders demo songs to WAV through the core. |
| `Tests/SPUTests` | Unit tests for the core. |

## Build and test

Requires Xcode (Swift 6 toolchain). No other dependencies.

```sh
scripts/test.sh                      # run the unit tests
swift run -c release spu-render      # render demo-all.wav
swift run -c release spu-render psg -o psg.wav --16bit
```

Demos: `all`, `psg`, `noise`, `pcm`, `adpcm`. Output is stereo WAV at the DS's native ~32,728 Hz. `--16bit` skips the DS's 10-bit output stage.

## Hardware accuracy

The core is written from GBATEK's DS sound documentation. Where GBATEK is ambiguous (for example, PSG duty 7 and start-up delays), behavior was checked against melonDS by observation only. No melonDS code is used, so the core isn't bound by the GPL.

Not emulated yet: the two capture units, SOUNDCNT output routing, the hold bit, and SOUNDBIAS (the standard 0x200 is assumed).
