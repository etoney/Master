# The Master

A master-channel mix analyser (VST3 / AU / Standalone) by **Castle Danger Music Company**.

The Master listens to the finished mix, compares it with genre profiles (what records in the genre measure), and gives plain-English mixing advice, pointing at Castle Danger plugins where one does the job (Dominatrix, Chop Shop, Flatliner, BuzzKill). On the **FIX IT** page, **AUTO FIX** applies the corrections itself (tone-match EQ, leveler, peak tamer, width / bass mono, hum notches, loudness into a true-peak limiter), and every correction can be overridden. It runs fully offline.

- **Measures:** integrated loudness, true peak, PLR (punch), loudness range, 10-band tonal balance, stereo width, correlation, mono low end, digital clipping, 50/60 Hz hum in the quiet parts
- **Genres:** Rock, Metal, Punk, Hard Rock, Pop, with **AUTO** genre detection (naive-Bayes match against the profiles) or a locked genre
- **The Verdict:** a 0-100 mix score plus FIX / CHECK / GOOD items, each with what to do and which plugin helps
- **Profiles:** factory pack compiled in (`Profiles/factory.json`), user packs layered on top. Admins update them in bulk with `TheMasterProfiler` or by capturing references inside the DAW. See `ADMIN.txt`.
- **FIX IT:** `Source/FixEngine.h` (the chain, transparent at neutral, constant latency) and `Source/Cure.h` (the AUTO FIX planner). The output is analysed too, so the report can show YOUR MIX vs AFTER FIXES; a second AUTO FIX fine-tunes loudness from the measured result. Overrides are ordinary automatable parameters; the AUTO plan is saved with the project so each section can be reverted.
- **MY PLUGINS:** `Source/Chain.h` prescribes a master-bus chain of Castle Danger plugins (Chop Shop, Dominatrix) with exact settings and a reason for each; `Source/CDLink.h` (Castle Danger Link, shared with the other plugins) finds them on 127.0.0.1, checks their order from level fingerprints, and sets their controls on APPLY; REFINE fine-tunes from the measured result.
- Analysis runs on a background thread. Live playback never waits on it; offline bounces are analysed in full. The report is saved with the project.

> The factory profiles in 1.0.0 are **seed estimates**. Rebuild them from real reference songs with `TheMasterProfiler` (see `ADMIN.txt`).

See `controls.txt` for the user guide and `readme.txt` for installing.

## Building

Pushing to `main` runs GitHub Actions, which builds macOS (VST3 + AU + Standalone, universal) and Windows (VST3 + Standalone), then:

- uploads the builds and the `TheMasterProfiler` admin tool as workflow artifacts, and
- pushes the release zips to the `downloads-macos` / `downloads-windows` branches.

A Linux job runs the headless checks in `test/snapshot.cpp`:

- analyser accuracy (LUFS at 44.1/48/96 kHz, true peak, LRA, pink-noise band balance, stereo, clipping, hum, no false hum)
- profile packs, the genre detector, building and pooling references, import/export/restore
- the advice and plugin references
- every fix stage (transparency, EQ, limiter ceilings, bass mono, width, hum notch, peak tamer, leveler, match), the AUTO FIX planner, and AUTO FIX end to end (score before vs after, loudness on target, override + revert, save/restore)
- pass-through, background analysis, offline-bounce completeness, state save/restore

It also renders the editor to PNGs (the `gui-snapshots` artifact).

Local build:

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target TheMaster_VST3 TheMasterProfiler
```

Built with JUCE 8.0.15 (same as Dominatrix, Chop Shop, DREAD, Possessed and BuzzKill).
