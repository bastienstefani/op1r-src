# Writing op1r plugins (synth engines and effects)

A plugin is one folder with a `plugin.json` and C code. It becomes a new synth engine or FX in the OP-1's lists.
You can write and test it on a PC; the op1r maintainers build it into a release. License: like all of op1r,
GPL-3.0-or-later with the OP-1 firmware permission (see `LICENSE` and `NOTICE` in the repository root).

## Quick start

```sh
cp -r template/synth plugins/mysynth          # or template/fx
# rename mysynth.c, the symbol (ENGINE_mysynth / FX_myfx) and "name" in plugin.json
python3 harness/audio.py mysynth              # -> harness/out/audio_mysynth.wav: listen to it
python3 harness/audio.py mysynth --knobs 0,32767,8000,20000
```

You need a C compiler and Python 3 (with numpy only for some of the other tests). `audio.py` builds your plugin
with address and undefined-behaviour sanitizers: out-of-bounds writes and integer overflows are reported there,
before they can crash an OP-1.

- **Synths** play a short phrase (velocities 127 and 64 alternating). Engines that use the slot's sampler
  recording get a 2 s synthetic recording.
- **FX** process plucked saw notes followed by 1.5 s of silence, so you can hear tails and feedback.

## plugin.json

```json
{ "name": "mysynth", "kind": "synth", "order": 99, "symbol": "ENGINE_mysynth",
  "sources": ["mysynth.c"], "defaults": [4000, 12000, 6000, 24000] }
```

- `name`: shown on the OP-1; lowercase, at most 15 characters, not a stock type name.
- `kind`: `synth` or `fx`.
- `order`: the plugin's number. It is stored in every saved preset, so it must never change once released. Use 99
  in your submission; the maintainers assign the final number.
- `symbol`: your `op1_engine_t` / `op1_fx_t` object; `sources`: C files, the first one defines `symbol`.
- `defaults`: initial positions of the 4 knobs (blue, green, white, red), 0..32767.
- Optional: `page2_defaults` (FX second-page knobs), `uses_sample` (synth reads the slot's sampler recording),
  `screen` (own screen, see below). All fields are documented in `plugins.py`.

## The API (op1_plugin.h)

Include only `op1_plugin.h`.

**Synth engine** (`op1_engine_t`): `note_on(self, params)`, `render(self, out[128], params)`, `note_off(self)`.
- 6 voices; `self` is the voice object: the first word belongs to the firmware, the next 32 bytes are yours.
- `OP1_PITCH_INC[n]`: phase increment per sample of this note (2^32 = one cycle).
  `op1_voice_vel_gain(self)`: note velocity as a Q15 gain.
- `params[0..3]`: the 4 knobs, 0..32767. The firmware's envelope and level are applied after `render`.

**Effect** (`op1_fx_t`): `init(pos)`, `render(pos, buf[64], params)` (in place).
- `pos`: 0 synth FX, 1 drum FX, 2 master FX (mono sum, mixed back as the wet part). Keep all state per position
  (e.g. `static state_t st[3]`): the same FX can run in several positions at once.
- Knobs: convert with `op1_fx_param_q15(params, i)` (raw words -> 0..32767).
- Memory: `op1_fx_scratch(pos)`, 176 KB per position (shared with the stock FX: it starts with foreign data, clear
  or track what you use).

**Helpers**: `OP1_SINE_TABLE` (8192 x int16), `op1_sat16`, `op1_mul16` (Q15). Second knob pages and latching
presses: the `press` field (see the comments in `op1_plugin.h`).

**Optional FX callbacks** (leave them 0 if you do not need them; `plugins/drive` uses all three):
- `render_stereo(l, r, params)`: the master FX gets both channels instead of `render` on the mono sum.
- `key(pos, knob, pressed)`: encoder presses on your FX page; return 1 if you handled it.
- `knob(pos, knob, delta, words)`: encoder turns on your FX page (`delta` 1024 per detent); edit the 8 raw `words`
  in place and return 1, or return 0 for the default handling. Changed words are saved with the sound. Words 0..3
  are raw (`op1_fx_param_q15` / `op1_fx_param_raw` convert), 5..7 are 0..32767.

## Rules (the code runs in the audio interrupt)

A crash, a very long block or an endless loop locks the OP-1 until it is switched off.

- **Integers only.** There is no FPU; float is emulated and far too slow. Use Q15 fixed point.
- **No division or modulo per sample**, no 64-bit arithmetic per sample (both are slow library calls). Per block
  (once per `render`) is fine. Multiply by a precomputed reciprocal instead.
- **No libc**: no malloc, printf, memcpy, memset. Write the loops.
- Keep the per-sample work small: the OP-1 has one DSP core shared by the instrument, FX, tape and UI.
  The resource monitor (clock screen + T1 on the OP-1) shows the load.
- Saturate before storing 16-bit values (`op1_sat16`).

## Own screen (optional)

Without `screen`, synths use the stock iter graphics and FX a generic page with the 4 knobs. For an own screen:
- an SVG (320 x 160, OP-1 line style; the file name must not clash with the OP-1's own `content/display` files),
- `op1_screen_t` with `init(scr)` (look up SVG nodes by id) and `update(scr, dt)` (every frame: move nodes, draw
  lines),
- `"screen": { "svg": "mysynth.svg", "symbol": "SCREEN_mysynth" }` in `plugin.json`.

Preview: `python3 harness/render.py mysynth out.png`. Look at `plugins/supersaw` (synth) or `plugins/tape` (FX).
Colours: blue #4D9EFF, green #00ED95, white #FFFFFF, red #FF3A5D, grey #6F7173. Line ends are flat and zero-length
lines are not drawn (draw a dot as two short crossing lines).

## Submitting

Open a pull request with your `plugins/<name>/` folder: sources, `plugin.json`, a WAV made with `audio.py` and,
with a screen, a preview PNG. Keep your copyright line in the sources; by submitting you license your plugin under the same terms as op1r
(GPL-3.0-or-later with the OP-1 firmware permission). The maintainers test it on hardware, assign
its number and include it in a release; users can still hide any plugin when they patch their firmware.
