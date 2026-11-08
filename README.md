# P2PhysFix

ASI plugin for **Prototype 2** (Steam build after the 29 September 2025 update). It does two things:

1. **Fixes a physics crash.** The game closes to desktop with `MSVCR90.dll` offset `0x3b6df` (memset on a NULL pointer). The cause is a race in the ravenphysics stack allocator. While an async physics step owns the free part of the shared scratch stack, main-thread physics queries allocate from the same memory. The plugin gives the main thread a private 4 MB stack while the step runs, and moves the physics stack to the CRT heap (6 MB → 32 MB).
2. **Optional borderless window.** It turns on the borderless mode built into the 2025 `prototype2.exe` (the same as `-windowed -borderless`).

All changes are applied in memory at startup. No game files are modified. The plugin checks the PE timestamps and the original bytes, and patches nothing on an unknown build. It writes `P2PhysFix.log` next to the game exe.

## What the code does (for reviewers)

- `DllMain` → `apply()`. It verifies `prototype2engine.dll` (base `0x10000000`, timestamp `0x68B09A3A`) and the original bytes at 8 addresses.
- It allocates one 4 KB RWX page (`VirtualAlloc`) for a small code cave of 148 bytes. The cave holds the stack-swap and init code; the plugin emits the bytes itself in `apply()`.
- It patches the 8 call sites with `VirtualProtect` + `memcpy` + `FlushInstructionCache`.
- Optional (`P2PhysFix.ini`): it hooks the engine's command-line check `HasOption` at `0x10673170` through a trampoline, and changes 6 bytes of the exe's window-mode selector at `0x401210`.
- No network access, no file access outside its own `.log`/`.ini` next to `prototype2.exe`, no other processes touched.

## Build (reproducible)

Requires [Zig 0.17.1](https://ziglang.org/download/) (zig-x86_64-windows-0.17.1.zip). No other dependencies.

```
zig cc -target x86-windows-gnu -shared -O2 -s -Wall -Wextra -o P2PhysFix.asi src/p2physfix.c -lkernel32 -luser32
```

You can also run `src/build.ps1`, which writes `build/P2PhysFix.asi`.

The release `P2PhysFix.asi` 1.1.0 has SHA-256
`3FAF1AB1AFEAE43B63AF32380A2EA696639E73154016660A3DF4D5BD3C3588C9`.
Building with the command above and Zig 0.17.1 gives a byte-identical file.

## Install

1. Install [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader). PrototypeFix already ships it as `binkw32.dll`.
2. Copy `P2PhysFix.asi` and `src/P2PhysFix.ini` next to `prototype2.exe`.
3. Optional: set `Borderless = 1` in the ini.

## Credits

- emoose — PrototypeFix
- ThirteenAG — Ultimate ASI Loader
- Made with Claude Code (Anthropic) and the universal-modder plugin

## License

MIT, see `LICENSE`.
