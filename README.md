# P2PhysFix

ASI plugin for **Prototype 2** (Steam build after the 29 September 2025 update).

1. **Physics crash fix (always on).** The game closes to desktop with `MSVCR90.dll` offset `0x3b6df` (memset on a NULL pointer). The cause is a race in the ravenphysics stack allocator. While an async physics step owns the free part of the shared scratch stack, main-thread physics queries allocate from the same memory. The plugin gives the main thread a private 4 MB stack while the step runs, and moves the physics stack to the CRT heap (6 MB → 32 MB).
2. **Borderless window (optional).** It turns on the borderless mode built into the 2025 `prototype2.exe` (the same as `-windowed -borderless`).
3. **Engine worker threads = 2 (on by default).** PrototypeFix's CPUThreadCountFix forces 4 workers. With 4, the engine's data slowly gets corrupted: after a few minutes the character drifts while flying, the camera jumps and Esc stops opening the pause menu. 2 is the engine's own quad-core value. The game still runs on all CPU threads; only the job pool has 2 workers.
4. **Cursor fixes (on by default).** The engine's unbalanced `ShowCursor` calls become absolute: no Windows arrow over the game, and no AMD overlay flicker. The cursor is confined to the game window while it is active.
5. **Optional:** don't minimize on alt+tab; extra engine command-line flags.

All changes are applied in memory at startup. No game files are modified. The plugin checks the PE timestamps and the original bytes, and patches nothing on an unknown build. It writes `P2PhysFix.log` next to the game exe.

## What the code does (for reviewers)

- **`apply()`.** It verifies `prototype2engine.dll` (base `0x10000000`, timestamp `0x68B09A3A`) and the original bytes at 8 addresses. It allocates one 4 KB RWX page (`VirtualAlloc`) for a small code cave of 148 bytes. The cave holds the stack-swap and init code; the plugin emits the bytes itself in `apply()`. It patches the 8 call sites with `VirtualProtect` + `memcpy` + `FlushInstructionCache`.
- **`apply_borderless()` / `apply_options()`** (opt-in via ini). They change 6 bytes of the exe's window-mode selector at `0x401210` and hook the engine's command-line check `HasOption` at `0x10673170` through a trampoline.
- **`apply_hide_cursor()`.** It redirects the engine's `ShowCursor` import (IAT `0x109E541C`) to a function that normalizes the cursor counter to -1/0. A helper thread waits for the game window (`prototypeWindowClass`) and subclasses it to re-apply the state on focus changes and to call `ClipCursor` while the game is active.
- **`apply_minimize()`** (opt-in). It replaces the engine's `ShowWindow(SW_MINIMIZE)` call at `0x10684848` with `add esp,8`.
- **`apply_workers()`.** It sets the worker-pool size at `0x1065FFAE` (`7f 07 b8 01 00 00 00` → `90 90 b8 02 00 00 00`). This is the same instruction that PrototypeFix's CPUThreadCountFix changes to 4.
- No network access, no file access outside its own `.log`/`.ini` next to `prototype2.exe`, no other processes touched.

## Build (reproducible)

Requires [Zig 0.17.1](https://ziglang.org/download/) (zig-x86_64-windows-0.17.1.zip). No other dependencies.

```
zig cc -target x86-windows-gnu -shared -O2 -s -Wall -Wextra -o P2PhysFix.asi src/p2physfix.c -lkernel32 -luser32
```

You can also run `src/build.ps1`, which writes `build/P2PhysFix.asi`.

The release `P2PhysFix.asi` 1.3.0 has SHA-256
`9A4E9CFA09E8EA3E3AF17287A4A9F00F17753EEC26BD2666C098A634471223C7`.
Building with the command above and Zig 0.17.1 gives a byte-identical file.

## Install

1. Install [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader). PrototypeFix already ships it as `binkw32.dll`.
2. Copy `P2PhysFix.asi` and `src/P2PhysFix.ini` next to `prototype2.exe`.
3. Optional: for a borderless window, set `Borderless = 1` and `MinimizeOnFocusLoss = 0` in the ini.

## Changelog

- **1.3.0** — engine worker threads forced to 2: fixes the slow drift / camera jumps / dead Esc caused by 4 workers.
- **1.2.0** — absolute cursor visibility (no Windows arrow, no AMD overlay flicker), cursor confinement, optional "don't minimize on alt+tab".
- **1.1.0** — optional borderless window (built-in 2025 exe mode), engine command-line flags.
- **1.0.0** — physics stack race fix, 32 MB physics stack on the CRT heap.

## Credits

- emoose — PrototypeFix
- ThirteenAG — Ultimate ASI Loader
- Made with Claude Code (Anthropic) and the universal-modder plugin

## License

MIT, see `LICENSE`.
