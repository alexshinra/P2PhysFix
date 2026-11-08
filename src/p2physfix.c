// P2PhysFix - Prototype 2 physics race crash fix (ASI plugin, loaded by Ultimate ASI Loader).
// MIT License. See README for the full write-up.
//
// Bug: ravenphysics' global StackAllocator. While an async physics step owns the free part [top..limit]
// (sLock = 1), main-thread physics queries keep allocating from the same `top`. Both threads write the same
// memory -> corrupted data -> an allocation loop exhausts the stack -> memset(NULL) in MSVCR90.dll.
// Fix (all in memory, game files untouched):
//   * on lock, swap top/limit to a private side stack; on unlock, swap them back;
//   * allocate the stack (+ side stack) with the CRT malloc instead of the small engine heap, 32 MB main stack.
// Target: prototype2engine.dll from the Steam update of 2025-09-29 (PE timestamp 0x68B09A3A, base 0x10000000).

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define P2PF_VERSION "1.1.0"

// prototype2.exe (Steam 2025-09-29 update) window-mode selector: 0 = fullscreen, 1 = borderless, 2 = windowed.
// It returns 1 only for "windowed borderless" on the command line; we can force it.
#define EXE_TS        0x68B09A20u
#define EXE_BASE      0x00400000u
#define EXE_MODE_FN   0x00401210u

#define ENGINE_NAME   "prototype2engine.dll"
#define ENGINE_TS     0x68B09A3Au
#define ENGINE_BASE   0x10000000u

#define G_BASE        0x10E55DE0u   // StackAllocator base
#define G_LOCK        0x10E55DE6u   // byte: sLock
#define G_LIMIT       0x10E55DF0u
#define G_TOP         0x10E55DF4u
#define IAT_MALLOC    0x109E52E4u   // MSVCR90!malloc
#define IAT_FREE      0x109E530Cu   // MSVCR90!free

#define MAIN_SIZE     0x02000000u   // 32 MB (vanilla: 6 MB)
#define ALT_SIZE      0x00400000u   // 4 MB side stack

static HANDLE g_log = INVALID_HANDLE_VALUE;

static void logf(const char *fmt, ...) {
  char buf[512];
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof(buf) - 3) n = (int)sizeof(buf) - 3;
  buf[n++] = '\r'; buf[n++] = '\n';
  if (g_log != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(g_log, buf, (DWORD)n, &w, NULL); }
}

static void open_log(void) {
  char path[MAX_PATH];
  DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
  while (len > 0 && path[len - 1] != '\\' && path[len - 1] != '/') len--;
  if (len + sizeof("P2PhysFix.log") >= MAX_PATH) return;
  memcpy(path + len, "P2PhysFix.log", sizeof("P2PhysFix.log"));
  g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

typedef struct { DWORD va; BYTE orig[16]; int len; const char *what; } Site;

static const Site SITES[] = {
  { 0x108D84F9u, {0x6a,0x04,0x56,0x50,0xe8,0x1e,0xa8,0xd6,0xff}, 9,  "StackAllocator init (heap alloc)" },
  { 0x108D8588u, {0xa1,0xe0,0x5d,0xe5,0x10,0x50,0xe8,0x4d,0xaa,0xd6,0xff,0x59,0xc3,0xcc}, 14, "StackAllocator shutdown (heap free)" },
  { 0x108E4793u, {0x00,0x00,0x60,0x00}, 4, "physics stack size (6 MB)" },
  { 0x108D8763u, {0xc6,0x05,0xe6,0x5d,0xe5,0x10,0x01}, 7, "lock with region: sLock=1" },
  { 0x108D87EBu, {0xc6,0x05,0xe6,0x5d,0xe5,0x10,0x01}, 7, "plain lock: sLock=1" },
  { 0x108D8793u, {0xc6,0x05,0xe6,0x5d,0xe5,0x10,0x00}, 7, "unlock with region (a): sLock=0" },
  { 0x108D87B0u, {0xc6,0x05,0xe6,0x5d,0xe5,0x10,0x00}, 7, "unlock with region (b): sLock=0" },
  { 0x108D882Bu, {0xc6,0x05,0xe6,0x5d,0xe5,0x10,0x00}, 7, "plain unlock: sLock=0" },
};
#define NSITES (int)(sizeof(SITES) / sizeof(SITES[0]))

// --- tiny x86 emitter -------------------------------------------------------
typedef struct { BYTE *p; BYTE *start; } Emit;
static void eb(Emit *e, BYTE b) { *e->p++ = b; }
static void ed(Emit *e, DWORD d) { memcpy(e->p, &d, 4); e->p += 4; }
static DWORD here(Emit *e) { return (DWORD)(ULONG_PTR)e->p; }
static void rel32_at(BYTE *at, DWORD from_va, DWORD to_va) { LONG r = (LONG)(to_va - (from_va + 5)); memcpy(at, &r, 4); }

static BOOL patch(DWORD va, const BYTE *bytes, int len) {
  DWORD old;
  if (!VirtualProtect((LPVOID)(ULONG_PTR)va, (SIZE_T)len, PAGE_EXECUTE_READWRITE, &old)) return FALSE;
  memcpy((void *)(ULONG_PTR)va, bytes, (size_t)len);
  VirtualProtect((LPVOID)(ULONG_PTR)va, (SIZE_T)len, old, &old);
  FlushInstructionCache(GetCurrentProcess(), (LPCVOID)(ULONG_PTR)va, (SIZE_T)len);
  return TRUE;
}

static void apply(void) {
  HMODULE eng = GetModuleHandleA(ENGINE_NAME);
  if (!eng) { logf("ERROR: %s is not loaded - nothing patched.", ENGINE_NAME); return; }
  if ((DWORD)(ULONG_PTR)eng != ENGINE_BASE) { logf("ERROR: %s loaded at %p, expected 0x%08X - nothing patched.", ENGINE_NAME, (void *)eng, ENGINE_BASE); return; }
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)eng + ((IMAGE_DOS_HEADER *)eng)->e_lfanew);
  DWORD ts = nt->FileHeader.TimeDateStamp;
  logf("%s timestamp 0x%08X", ENGINE_NAME, ts);
  if (ts != ENGINE_TS) { logf("Unsupported engine build (expected 0x%08X, Steam 2025-09-29 update) - nothing patched.", ENGINE_TS); return; }

  // Already patched on disk by p2physfix.ps1?
  if (memcmp((void *)(ULONG_PTR)SITES[2].va, "\x00\x00\x00\x02", 4) == 0 && *(BYTE *)(ULONG_PTR)SITES[3].va == 0xE8) {
    logf("Engine file is already patched (p2physfix.ps1) - in-memory patch not needed.");
    return;
  }
  for (int i = 0; i < NSITES; i++) {
    if (memcmp((void *)(ULONG_PTR)SITES[i].va, SITES[i].orig, (size_t)SITES[i].len) != 0) {
      logf("ERROR: unexpected bytes at 0x%08X (%s) - another mod patched it? Nothing patched.", SITES[i].va, SITES[i].what);
      return;
    }
  }

  BYTE *mem = (BYTE *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  if (!mem) { logf("ERROR: VirtualAlloc failed (%lu) - nothing patched.", GetLastError()); return; }
  DWORD vSavedTop = (DWORD)(ULONG_PTR)(mem + 0), vSavedLimit = vSavedTop + 4, vAltBase = vSavedTop + 8;

  Emit e = { mem + 0x20, mem };
  // cave_init(size): eax = malloc(size + ALT); altBase = eax + size
  DWORD caveInit = here(&e);
  eb(&e, 0x8b); eb(&e, 0x44); eb(&e, 0x24); eb(&e, 0x04);          // mov eax,[esp+4]
  eb(&e, 0x05); ed(&e, ALT_SIZE);                                   // add eax,ALT_SIZE
  eb(&e, 0x50);                                                     // push eax
  eb(&e, 0xff); eb(&e, 0x15); ed(&e, IAT_MALLOC);                   // call [malloc]
  eb(&e, 0x83); eb(&e, 0xc4); eb(&e, 0x04);                         // add esp,4
  eb(&e, 0x85); eb(&e, 0xc0);                                       // test eax,eax
  eb(&e, 0x74); eb(&e, 0x0c);                                       // jz ret
  eb(&e, 0x8b); eb(&e, 0x4c); eb(&e, 0x24); eb(&e, 0x04);          // mov ecx,[esp+4]
  eb(&e, 0x03); eb(&e, 0xc8);                                       // add ecx,eax
  eb(&e, 0x89); eb(&e, 0x0d); ed(&e, vAltBase);                     // mov [altBase],ecx
  eb(&e, 0xc3);                                                     // ret
  // cave_lock: sLock=1; save top/limit; top=altBase; limit=altBase+ALT
  DWORD caveLock = here(&e);
  eb(&e, 0xc6); eb(&e, 0x05); ed(&e, G_LOCK); eb(&e, 0x01);
  eb(&e, 0x50);
  eb(&e, 0xa1); ed(&e, G_TOP);    eb(&e, 0xa3); ed(&e, vSavedTop);
  eb(&e, 0xa1); ed(&e, G_LIMIT);  eb(&e, 0xa3); ed(&e, vSavedLimit);
  eb(&e, 0xa1); ed(&e, vAltBase); eb(&e, 0xa3); ed(&e, G_TOP);
  eb(&e, 0x05); ed(&e, ALT_SIZE); eb(&e, 0xa3); ed(&e, G_LIMIT);
  eb(&e, 0x58); eb(&e, 0xc3);
  // cave_unlock: sLock=0; restore top/limit
  DWORD caveUnlock = here(&e);
  eb(&e, 0xc6); eb(&e, 0x05); ed(&e, G_LOCK); eb(&e, 0x00);
  eb(&e, 0x50);
  eb(&e, 0xa1); ed(&e, vSavedTop);   eb(&e, 0xa3); ed(&e, G_TOP);
  eb(&e, 0xa1); ed(&e, vSavedLimit); eb(&e, 0xa3); ed(&e, G_LIMIT);
  eb(&e, 0x58); eb(&e, 0xc3);
  FlushInstructionCache(GetCurrentProcess(), mem, 4096);
  logf("code cave at %p (%d bytes)", (void *)mem, (int)(e.p - mem));

  BYTE buf[16];
  // 1) init: push esi x3 / call cave_init / nop   (caller still does add esp,0Ch)
  buf[0] = 0x56; buf[1] = 0x56; buf[2] = 0x56; buf[3] = 0xe8; rel32_at(buf + 4, SITES[0].va + 3, caveInit); buf[8] = 0x90;
  patch(SITES[0].va, buf, 9);
  // 2) shutdown: push [base] / call [free] / pop ecx / ret
  {
    BYTE s[14] = {0xff,0x35, 0,0,0,0, 0xff,0x15, 0,0,0,0, 0x59, 0xc3};
    DWORD a = G_BASE, f = IAT_FREE; memcpy(s + 2, &a, 4); memcpy(s + 8, &f, 4);
    patch(SITES[1].va, s, 14);
  }
  // 3) stack size
  { DWORD sz = MAIN_SIZE; patch(SITES[2].va, (BYTE *)&sz, 4); }
  // 4) sLock writes -> call cave / nop nop
  for (int i = 3; i < NSITES; i++) {
    DWORD tgt = (SITES[i].orig[6] == 0x01) ? caveLock : caveUnlock;
    buf[0] = 0xe8; rel32_at(buf + 1, SITES[i].va, tgt); buf[5] = 0x90; buf[6] = 0x90;
    patch(SITES[i].va, buf, 7);
  }
  for (int i = 0; i < NSITES; i++) logf("patched 0x%08X  %s", SITES[i].va, SITES[i].what);
  logf("physics race fix applied: main stack %u MB (CRT heap), side stack %u MB.", MAIN_SIZE >> 20, ALT_SIZE >> 20);
}

// --- settings (P2PhysFix.ini next to prototype2.exe) -----------------------
static char g_ini[MAX_PATH];
static void ini_path(void) {
  DWORD len = GetModuleFileNameA(NULL, g_ini, MAX_PATH);
  while (len > 0 && g_ini[len - 1] != '\\' && g_ini[len - 1] != '/') len--;
  if (len + sizeof("P2PhysFix.ini") >= MAX_PATH) { g_ini[0] = 0; return; }
  memcpy(g_ini + len, "P2PhysFix.ini", sizeof("P2PhysFix.ini"));
}
static int ini_int(const char *sec, const char *key, int def) {
  return g_ini[0] ? (int)GetPrivateProfileIntA(sec, key, def, g_ini) : def;
}

// --- engine command-line options --------------------------------------------
// prototype2engine.dll: char __cdecl HasOption(const char *name) at 0x10673170 - checks the parsed command line
// (87 callers: "windowed", "NoSleepOnLostFocus", ...). We hook it to add options from P2PhysFix.ini.
#define ENG_HASOPTION 0x10673170u
typedef char (__cdecl *HasOption_t)(const char *);
static HasOption_t g_hasopt_orig;
static char g_opts[512];          // space-separated option names that HasOption should report as present

static void add_opt(const char *name) {
  size_t a = strlen(g_opts), b = strlen(name);
  if (a + b + 2 >= sizeof(g_opts)) return;
  if (a) g_opts[a++] = ' ';
  memcpy(g_opts + a, name, b + 1);
}
static int opt_listed(const char *name) {
  size_t n = strlen(name);
  const char *p = g_opts;
  while (*p) {
    while (*p == ' ') p++;
    const char *q = p; while (*q && *q != ' ') q++;
    if ((size_t)(q - p) == n && _strnicmp(p, name, n) == 0) return 1;
    p = q;
  }
  return 0;
}
static char __cdecl hasopt_hook(const char *name) {
  if (name && g_opts[0] && opt_listed(name)) return 1;
  return g_hasopt_orig(name);
}

static void apply_options(BYTE *tramp) {
  if (!g_opts[0]) return;
  static const BYTE orig[] = {0x53, 0x8b, 0x5c, 0x24, 0x08, 0x56, 0x8b, 0xc3};   // push ebx / mov ebx,[esp+8] / push esi / mov eax,ebx
  if (memcmp((void *)(ULONG_PTR)ENG_HASOPTION, orig, sizeof(orig)) != 0) {
    logf("Options: HasOption at 0x%08X has unexpected bytes - options not injected.", ENG_HASOPTION);
    return;
  }
  memcpy(tramp, orig, sizeof(orig));                                             // trampoline: original 8 bytes + jmp back
  tramp[8] = 0xe9; rel32_at(tramp + 9, (DWORD)(ULONG_PTR)(tramp + 8), ENG_HASOPTION + 8);
  FlushInstructionCache(GetCurrentProcess(), tramp, 16);
  g_hasopt_orig = (HasOption_t)(void *)tramp;
  BYTE j[8] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
  rel32_at(j + 1, ENG_HASOPTION, (DWORD)(ULONG_PTR)&hasopt_hook);
  patch(ENG_HASOPTION, j, 8);
  logf("Options: engine command-line options forced on: %s", g_opts);
}

static void apply_borderless(void) {
  if (!ini_int("Window", "Borderless", 0)) { logf("Borderless: off (P2PhysFix.ini [Window] Borderless=0)"); return; }
  add_opt("windowed");   // engine side: D3D windowed instead of exclusive fullscreen
  HMODULE exe = GetModuleHandleA(NULL);
  if ((DWORD)(ULONG_PTR)exe != EXE_BASE) { logf("Borderless: exe base %p unexpected - skipped.", (void *)exe); return; }
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
  if (nt->FileHeader.TimeDateStamp != EXE_TS) { logf("Borderless: unsupported prototype2.exe build (0x%08X) - skipped.", (unsigned)nt->FileHeader.TimeDateStamp); return; }
  static const BYTE orig[] = {0x57, 0x6a, 0x00, 0x8b, 0xc6, 0xb9, 0x7c, 0x31, 0x40, 0x00};   // push edi / push 0 / mov eax,esi / mov ecx,"windowed"
  if (memcmp((void *)(ULONG_PTR)EXE_MODE_FN, orig, sizeof(orig)) != 0) {
    logf("Borderless: window-mode code not found at 0x%08X (exe still encrypted or patched) - skipped.", EXE_MODE_FN);
    return;
  }
  static const BYTE ret1[] = {0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3};                          // mov eax,1 / ret
  patch(EXE_MODE_FN, ret1, sizeof(ret1));
  logf("Borderless: on - game uses its built-in borderless window at desktop resolution (%dx%d).",
       GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(inst);
    open_log();
    ini_path();
    logf("P2PhysFix " P2PF_VERSION " - Prototype 2 physics race crash fix");
    apply();
    apply_borderless();
    {
      char extra[256] = {0};
      if (g_ini[0]) GetPrivateProfileStringA("CommandLine", "Options", "", extra, sizeof(extra), g_ini);
      for (char *t = strtok(extra, " ,;\t"); t; t = strtok(NULL, " ,;\t")) add_opt(t);
    }
    if (g_opts[0]) {
      HMODULE eng = GetModuleHandleA(ENGINE_NAME);
      IMAGE_NT_HEADERS *nt = eng ? (IMAGE_NT_HEADERS *)((BYTE *)eng + ((IMAGE_DOS_HEADER *)eng)->e_lfanew) : NULL;
      BYTE *tramp = NULL;
      if (!nt || (DWORD)(ULONG_PTR)eng != ENGINE_BASE || nt->FileHeader.TimeDateStamp != ENGINE_TS)
        logf("Options: unsupported or missing %s - options not injected.", ENGINE_NAME);
      else if (!(tramp = (BYTE *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)))
        logf("Options: VirtualAlloc failed - options not injected.");
      else
        apply_options(tramp);
    }
    if (g_log != INVALID_HANDLE_VALUE) { CloseHandle(g_log); g_log = INVALID_HANDLE_VALUE; }
  }
  return TRUE;
}
