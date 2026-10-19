// ============================================================================
//  Ishimura Patch  (ishimurapatch.asi)  -  Dead Space (2023)
// ----------------------------------------------------------------------------
//  It times the game's D3D12 GPU memory requests and the residency waits
//  inside them, and logs the frames they slow down. 0.2.0 adds the first fix,
//  SUBALLOCATION (off by default, F9 switches it live): the game's small
//  committed resources are created as placed resources inside a few large
//  pre-made heaps instead.
//
//  Why: at streaming boundaries (the lift test spot) the game creates
//  hundreds of small GPU resources in one frame. D3D12 makes each one
//  resident at creation and blocks the creating thread on the paging fence
//  (D3D12Core!CDevice::MakeResident_Worker -> PerNodePagingOperation::Wait),
//  about 0.3 ms of frame per request (ETW trace, 10/18). This build splits
//  that cost into the request and the wait, per thread and per frame, before
//  any fix is attempted.
//
//  What is hooked (none of it is game code; Dead Space.exe is Denuvo-
//  protected and stays untouched):
//   - d3d12.dll!D3D12CreateDevice (MinHook), to find the device.
//   - The device's vtable: every Create*Resource / CreateHeap* variant,
//     MakeResident, Evict, EnqueueMakeResident. Slots come from vtindex.c.
//   - Present / Present1 (MinHook, on the code), the frame boundary, found
//     through a throwaway 8x8 swap chain on a hidden window.
//   - D3DKMTWaitForSynchronizationObjectFromCpu (MinHook), the kernel wait
//     the residency fence goes through.
//
//  Suballocation: in-game (0.1.1, lift pass) the worst frame was 98.8 ms,
//  447 committed creates at ~0.2 ms each (60% of it the residency wait) on
//  one job thread, next to 438 placed creates at ~0.01 ms each. Over a
//  session the game made 20,622 committed creates (6.8 s of thread time),
//  nearly all DEFAULT-heap, CREATE_NOT_ZEROED, flags 0 or UAV, <= 4 MB. Only
//  that shape is redirected: NOT_ZEROED means reused memory is acceptable,
//  and no render targets or depth buffers means no placed-resource
//  initialization rules apply. A resource's pages go back to the pool when
//  D3D12 destroys it (a tracker set with SetPrivateDataInterface is released
//  then). Anything that fails falls back to the committed path.
//  0.2.1, from the 0.2.0 A/B: with the fix on, the creates left committed
//  were textures over 4 MB (~1 ms each, 389 ms a minute) and 64 KB READBACK
//  buffers in batches of ~50 (15-17 ms frames). The size cap is now 16 MB,
//  UPLOAD and READBACK buffers get their own pools, and blocks are made
//  ahead of need on a background thread instead of mid-burst.
//
//  Logging (ishimurapatch.ini): 1 = what was hooked, a HITCH line per slow
//  frame, a STATS line every 30 s. 2 = also every request and every frame,
//  written by a background thread from a lock-free ring so the hooks never
//  touch the file; the capture key saves the last minute to its own file.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <share.h>
#include <time.h>
#include "third_party/minhook/include/MinHook.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "version.lib")

static const char* MOD_VERSION = "0.3.1";

extern "C" {
extern const int VT_GetResourceAllocationInfo, VT_CreateCommittedResource, VT_CreateHeap,
    VT_CreatePlacedResource, VT_CreateReservedResource, VT_MakeResident, VT_Evict,
    VT_EnqueueMakeResident, VT_CreateCommittedResource1, VT_CreateHeap1, VT_CreateReservedResource1,
    VT_CreateCommittedResource2, VT_CreatePlacedResource1, VT_CreateCommittedResource3,
    VT_CreatePlacedResource2, VT_CreateReservedResource2, VT_Present, VT_Present1;
}

// ----------------------------------------------------------------------------
// Files and config
// ----------------------------------------------------------------------------
struct Files
{
    HMODULE module = NULL;
    char    iniPath[MAX_PATH] = {};
    char    logPath[MAX_PATH] = {};
    bool    iniCreated = false;
};
static Files g_files;

struct Config
{
    int  logging = 1;
    int  hitchMs = 20;
    char captureKey[32] = "F8";
    int  suballocate = 1;
    char toggleKey[32] = "F9";
    int  blockMB = 128;
    int  prewarmBlocks = 4;
    int  maxResourceKB = 16384;
    int  maxPoolMB = 4096;
    int  hostBlockMB = 32;
    int  maxHostPoolMB = 512;
    int  probe = 0;
    struct { char cls[96], fld[96], val[64]; } engine[16];   // [Engine] Class.Field = value
    int  nEngine = 0;
};
static Config g_cfg;

static const char* DEFAULT_INI =
"; ============================================================================\n"
";  Dead Space (2023) Ishimura Patch - configuration\n"
";\n"
";  Fixes the traversal stutter at streaming boundaries (walking off a lift,\n"
";  through doors, down hallways). Nothing on disk is changed.\n"
";\n"
";  Every setting here is optional. If you run into problems, delete this\n"
";  file and it will be recreated with defaults on the next launch.\n"
";\n"
";  Edit with any text editor. Changes take effect within about a second\n"
";  of saving - even while the game is running - except the settings marked\n"
";  (read at launch). Check ishimurapatch.log (same folder) to confirm what\n"
";  was applied.\n"
"; ============================================================================\n"
"\n"
"; ============================================================================\n"
";  FIXES\n"
"; ============================================================================\n"
"\n"
"[Fix]\n"
"\n"
"; Most of the stutter comes from the game asking Windows for hundreds of\n"
"; small GPU memory blocks in one frame, each one a slow round trip. With\n"
"; this on, those small textures and buffers are carved out of a few large\n"
"; blocks made in advance instead. (Lift test: worst frame ~106 -> ~18 ms.)\n"
"; Switching it applies to textures made from then on.\n"
"; 1 = fixed (default), 0 = stock game behavior\n"
"Suballocate = 1\n"
"\n"
"; Press this key while playing to switch Suballocate on or off (logged),\n"
"; for comparing the two in one session. F1-F12 or a letter; empty = off.\n"
"ToggleKey = F9\n"
"\n"
"; (read at launch) The large blocks in graphics memory: their size (MB,\n"
"; 16-256), how many are made when the game starts (0-16; more are added\n"
"; in the background before they're needed), and the most they may total\n"
"; (MB, 256-16384). At that limit, further requests are left as the game\n"
"; made them, so this caps the extra graphics memory the fix can hold.\n"
"BlockMB = 128\n"
"PrewarmBlocks = 4\n"
"MaxPoolMB = 4096\n"
"\n"
"; (read at launch) The largest single resource carved out of a block (KB,\n"
"; 64-16384; never more than half a block). Larger ones stay as the game\n"
"; made them.\n"
"MaxResourceKB = 16384\n"
"\n"
"; (read at launch) The same for the small buffers the CPU and GPU pass\n"
"; data through (system memory, not graphics memory): block size (MB,\n"
"; 4-64) and the most they may total (MB, 16-2048).\n"
"HostBlockMB = 32\n"
"MaxHostPoolMB = 512\n"
"\n"
"\n"
"; ============================================================================\n"
";  ENGINE SETTINGS - the game's own options, set in the running game\n"
"; ============================================================================\n"
"\n"
"[Engine]\n"
"\n"
"; Written into the running game a few seconds after it starts, and kept\n"
"; that way (logged). One per line, as Class.Field = value. Delete a line,\n"
"; or put ; in front of it, and the game's own value is put back.\n"
";\n"
"; ForceLoadStreamingImmediate: the game normally stops a frame to finish\n"
"; loading a texture it needs right now. Off, the frame goes on with a\n"
"; slightly lower-detail version for a few frames until the sharp one is\n"
"; in. With [Fix] on this takes the lift test's worst frame ~18 -> ~12 ms.\n"
"TextureStreamingSettings.ForceLoadStreamingImmediate = 0\n"
"\n"
"\n"
"[Advanced]\n"
"\n"
"; Write a log to ishimurapatch.log (a fresh file every launch; a session's\n"
"; log stops at 256 MB)\n"
";  0 = off\n"
";  1 = on (default): what was hooked, one HITCH line for every frame at\n"
";      least HitchMs long, and a STATS line every 30 seconds\n"
";  2 = verbose (read at launch): also every GPU memory request and every\n"
";      frame, stamped in ms since the game started. Roughly 10-15 MB a\n"
";      minute; only worth turning on for a test run.\n"
"Logging = 1\n"
"\n"
"; A frame at least this long (ms) gets a HITCH line.\n"
"HitchMs = 20\n"
"\n"
"; Press this key right after a stutter and the mod marks the moment in the\n"
"; log and saves the last minute of it to its own file,\n"
"; ishimurapatch_capture_<date>_<time>.log, which a relaunch does not\n"
"; overwrite. Works at Logging = 2 only; inert at the others. F1-F12 or a\n"
"; letter; empty = off.\n"
"CaptureKey = F8\n"
"\n"
"; DEVELOPMENT (read at launch). Runs ishimurapatch_probe.dll (beside this\n"
"; file) once each time it changes, from 20 seconds after the game starts:\n"
"; reverse-engineering code that is rebuilt while the game runs. Without\n"
"; the file this does nothing.\n"
"; 0 = off (default), 1 = on\n"
"Probe = 0\n";

// One reader for boot and live (the Mouse Fix rule). Every key falls back to
// `fb` when it is missing: boot passes the declared defaults (a fresh
// Config); live passes the same defaults except Logging, which keeps its
// last value so a deleted Logging line can't silence a session's log. `c`
// and `fb` must be different objects. Ranges are checked here, for both.
static void ReadConfig(Config& c, const Config& fb)
{
    const char* ini = g_files.iniPath;
    c.suballocate   = GetPrivateProfileIntA("Fix", "Suballocate", fb.suballocate, ini) ? 1 : 0;
    GetPrivateProfileStringA("Fix", "ToggleKey", fb.toggleKey, c.toggleKey, sizeof(c.toggleKey), ini);
    c.blockMB       = GetPrivateProfileIntA("Fix", "BlockMB", fb.blockMB, ini);
    c.prewarmBlocks = GetPrivateProfileIntA("Fix", "PrewarmBlocks", fb.prewarmBlocks, ini);
    c.maxPoolMB     = GetPrivateProfileIntA("Fix", "MaxPoolMB", fb.maxPoolMB, ini);
    c.maxResourceKB = GetPrivateProfileIntA("Fix", "MaxResourceKB", fb.maxResourceKB, ini);
    c.hostBlockMB   = GetPrivateProfileIntA("Fix", "HostBlockMB", fb.hostBlockMB, ini);
    c.maxHostPoolMB = GetPrivateProfileIntA("Fix", "MaxHostPoolMB", fb.maxHostPoolMB, ini);
    {
        // [Engine]: every "Class.Field = value" line
        static char sec[8192];
        DWORD len = GetPrivateProfileSectionA("Engine", sec, sizeof(sec), ini);
        c.nEngine = 0;
        for (char* p = sec; len && *p && c.nEngine < 16; p += strlen(p) + 1)
        {
            if (*p == ';' || *p == '#') continue;
            auto& e = c.engine[c.nEngine];
            char key[200] = {};
            if (sscanf_s(p, " %199[^=] = %63s", key, (unsigned)sizeof(key), e.val, (unsigned)sizeof(e.val)) != 2) continue;
            for (char* k = key + strlen(key); k > key && (k[-1] == ' ' || k[-1] == '\t'); ) *--k = 0;
            char* dot = strchr(key, '.');
            if (!dot) continue;
            *dot = 0;
            strncpy_s(e.cls, sizeof(e.cls), key, _TRUNCATE);
            strncpy_s(e.fld, sizeof(e.fld), dot + 1, _TRUNCATE);
            c.nEngine++;
        }
    }
    c.logging = GetPrivateProfileIntA("Advanced", "Logging", fb.logging, ini);
    c.hitchMs = GetPrivateProfileIntA("Advanced", "HitchMs", fb.hitchMs, ini);
    GetPrivateProfileStringA("Advanced", "CaptureKey", fb.captureKey, c.captureKey, sizeof(c.captureKey), ini);
    c.probe   = GetPrivateProfileIntA("Advanced", "Probe", fb.probe, ini) ? 1 : 0;

    if (c.logging < 0 || c.logging > 2) c.logging = fb.logging;
    if (c.hitchMs < 1) c.hitchMs = fb.hitchMs;
    if (c.blockMB < 16 || c.blockMB > 256) c.blockMB = fb.blockMB;
    if (c.prewarmBlocks < 0 || c.prewarmBlocks > 16) c.prewarmBlocks = fb.prewarmBlocks;
    if (c.maxResourceKB < 64 || c.maxResourceKB > 16384) c.maxResourceKB = fb.maxResourceKB;
    if (c.maxPoolMB < 256 || c.maxPoolMB > 16384) c.maxPoolMB = fb.maxPoolMB;
    if (c.maxPoolMB < c.blockMB) c.maxPoolMB = c.blockMB;
    if (c.hostBlockMB < 4 || c.hostBlockMB > 64) c.hostBlockMB = fb.hostBlockMB;
    if (c.maxHostPoolMB < 16 || c.maxHostPoolMB > 2048) c.maxHostPoolMB = fb.maxHostPoolMB;
    if (c.maxHostPoolMB < c.hostBlockMB) c.maxHostPoolMB = c.hostBlockMB;
}

static void LoadConfig()
{
    char dir[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(g_files.module, dir, MAX_PATH);
    char* slash = (n && n < MAX_PATH) ? strrchr(dir, '\\') : NULL;
    if (!slash) return;
    slash[1] = 0;
    _snprintf_s(g_files.iniPath, sizeof(g_files.iniPath), _TRUNCATE, "%sishimurapatch.ini", dir);
    _snprintf_s(g_files.logPath, sizeof(g_files.logPath), _TRUNCATE, "%sishimurapatch.log", dir);

    if (GetFileAttributesA(g_files.iniPath) == INVALID_FILE_ATTRIBUTES)
    {
        FILE* f = NULL;
        if (fopen_s(&f, g_files.iniPath, "wb") == 0 && f)
        {
            fputs(DEFAULT_INI, f);
            fclose(f);
            g_files.iniCreated = true;
        }
    }
    ReadConfig(g_cfg, Config());    // every key; a missing one = its default
}

// ----------------------------------------------------------------------------
// Time: everything is QPC, the clock PresentMon and ETW use, so a log lines
// up with their captures. Stamps are ms since the mod started (qpc0, logged).
// ----------------------------------------------------------------------------
static LONGLONG g_qpcFreq = 1, g_qpc0 = 0;
static inline LONGLONG Now() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
static inline double Ms(LONGLONG ticks) { return ticks * 1000.0 / (double)g_qpcFreq; }

// ----------------------------------------------------------------------------
// Logging (the Mouse Fix log: one critical section, opened once, shared for
// reading, flushed per line, capped per session)
// ----------------------------------------------------------------------------
struct LazyCs
{
    CRITICAL_SECTION cs;
    volatile LONG    state = 0;   // 0 = uninit, 1 = initializing, 2 = ready
};
static void EnsureCs(LazyCs& l)
{
    LONG st = InterlockedCompareExchange(&l.state, 1, 0);
    if (st == 0)
    {
        InitializeCriticalSection(&l.cs);
        InterlockedExchange(&l.state, 2);
    }
    else while (l.state != 2) Sleep(0);
}

struct LogState
{
    LazyCs cs;
    FILE*  file = NULL;
    long   bytes = 0;
    bool   capped = false;
};
static LogState g_log;
static const long LOG_CAP_BYTES = 256L * 1024 * 1024;

// stampTicks < 0: stamp with now. Stamps are written at Logging 2 only.
static void LogWriteAt(LONGLONG stampTicks, const char* fmt, va_list ap)
{
    EnsureCs(g_log.cs);
    EnterCriticalSection(&g_log.cs.cs);
    if (g_log.capped) { LeaveCriticalSection(&g_log.cs.cs); return; }
    if (!g_log.file) g_log.file = _fsopen(g_files.logPath, "w", _SH_DENYWR);
    if (g_log.file)
    {
        int n = 0;
        if (g_cfg.logging == 2)
            n += fprintf(g_log.file, "%10.3f| ", Ms((stampTicks < 0 ? Now() : stampTicks) - g_qpc0));
        n += vfprintf(g_log.file, fmt, ap);
        fputc('\n', g_log.file); n += 1;
        if (n > 0) g_log.bytes += n;
        if (g_log.bytes > LOG_CAP_BYTES)
        {
            fprintf(g_log.file, "log stopped (%ld MB size cap reached): log restarts at next game launch\n",
                    LOG_CAP_BYTES / (1024 * 1024));
            g_log.capped = true;
        }
        fflush(g_log.file);
    }
    LeaveCriticalSection(&g_log.cs.cs);
}
static void Log(const char* fmt, ...)
{
    if (!g_cfg.logging || !g_files.logPath[0]) return;
    va_list ap; va_start(ap, fmt); LogWriteAt(-1, fmt, ap); va_end(ap);
}
static void LogAt(LONGLONG stampTicks, const char* fmt, ...)
{
    if (!g_cfg.logging || !g_files.logPath[0]) return;
    va_list ap; va_start(ap, fmt); LogWriteAt(stampTicks, fmt, ap); va_end(ap);
}

#include "frostbite.h"   // type records, memory scans, live settings objects

// ----------------------------------------------------------------------------
// Threads: a slot per thread that calls a hook, holding its name and this
// frame's totals. The names are the engine's own (Job0-Job13, RenderLoop...).
// ----------------------------------------------------------------------------
struct ThreadSlot
{
    DWORD          tid;
    char           name[48];
    volatile LONG64 createTicks;  // this frame, inside create calls
    volatile LONG64 waitTicks;    // this frame, residency waits inside them
    volatile LONG   creates;
};
static const int MAX_SLOTS = 256;
static ThreadSlot     g_slots[MAX_SLOTS];
static volatile LONG  g_slotCount = 0;
static thread_local int      t_slot = -1;
static thread_local int      t_inCreate = 0;
static thread_local LONGLONG t_waitTicks = 0;   // running total of this thread's KMT waits

typedef HRESULT (WINAPI* PFN_GetThreadDescription)(HANDLE, PWSTR*);
static PFN_GetThreadDescription p_GetThreadDescription;

static int MySlot()
{
    if (t_slot >= 0) return t_slot;
    LONG i = InterlockedIncrement(&g_slotCount) - 1;
    if (i >= MAX_SLOTS) { t_slot = MAX_SLOTS - 1; return t_slot; }   // overflow shares the last
    ThreadSlot& s = g_slots[i];
    s.tid = GetCurrentThreadId();
    s.name[0] = 0;
    PWSTR w = NULL;
    if (p_GetThreadDescription && SUCCEEDED(p_GetThreadDescription(GetCurrentThread(), &w)) && w)
    {
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.name, sizeof(s.name), NULL, NULL);
        LocalFree(w);
    }
    if (!s.name[0]) _snprintf_s(s.name, sizeof(s.name), _TRUNCATE, "tid%lu", s.tid);
    t_slot = i;
    return i;
}

// ----------------------------------------------------------------------------
// This frame's totals (between one Present and the next)
// ----------------------------------------------------------------------------
enum Kind : unsigned short
{
    K_COMMITTED, K_COMMITTED1, K_COMMITTED2, K_COMMITTED3, K_HEAP, K_HEAP1,
    K_PLACED, K_PLACED1, K_PLACED2, K_RESERVED, K_RESERVED1, K_RESERVED2,
    K_MAKERESIDENT, K_EVICT, K_ENQUEUE, K_FRAME,
    K_SUB, K_SUB1, K_SUB2, K_SUB3,   // a committed request made placed by the fix
    K_COUNT
};
static const char* KIND_NAME[K_COUNT] = {
    "committed", "committed1", "committed2", "committed3", "heap", "heap1",
    "placed", "placed1", "placed2", "reserved", "reserved1", "reserved2",
    "MakeResident", "Evict", "EnqueueMakeResident", "FRAME",
    "sub", "sub1", "sub2", "sub3" };
enum Cat { C_COMMITTED, C_HEAP, C_PLACED, C_RESERVED, C_RESIDENCY, C_SUB, C_FRAME, C_COUNT };
static Cat CatOf(Kind k)
{
    switch (k)
    {
    case K_COMMITTED: case K_COMMITTED1: case K_COMMITTED2: case K_COMMITTED3: return C_COMMITTED;
    case K_HEAP: case K_HEAP1:                                                 return C_HEAP;
    case K_PLACED: case K_PLACED1: case K_PLACED2:                             return C_PLACED;
    case K_RESERVED: case K_RESERVED1: case K_RESERVED2:                       return C_RESERVED;
    case K_SUB: case K_SUB1: case K_SUB2: case K_SUB3:                         return C_SUB;
    case K_FRAME:                                                              return C_FRAME;
    default:                                                                   return C_RESIDENCY;
    }
}

struct FrameAcc
{
    volatile LONG   count[C_COUNT];
    volatile LONG64 createTicks;      // thread-time inside create calls
    volatile LONG64 waitTicks;        // residency waits inside them
    volatile LONG64 otherWaitTicks;   // KMT waits outside any create call
    volatile LONG64 bytes;
};
static FrameAcc g_frame;

// ----------------------------------------------------------------------------
// The event ring (Logging 2): hooks claim a slot with one interlocked add,
// fill it and publish it with its sequence number; the writer thread drains
// it into the log. A lapped slot is counted as dropped, never blocked on.
// ----------------------------------------------------------------------------
struct Event
{
    volatile LONG64 seq;      // index + 1 once published
    LONGLONG qpc, dur, wait, extra;
    UINT64   bytes, width;
    DWORD    tid;
    int      slot;
    unsigned short kind, heapType, dim, mips;
    UINT32   heapFlags, resFlags, format, count, height, depth;
    LONG     hr;
};
static const LONG64 RING_SIZE = 1 << 17;   // ~65 s at the busiest rates seen
static Event*         g_ring = NULL;
static volatile LONG64 g_ringHead = 0;

static Event* RingClaim(LONG64& idx)
{
    idx = InterlockedIncrement64(&g_ringHead) - 1;
    Event* e = &g_ring[idx & (RING_SIZE - 1)];
    InterlockedExchange64(&e->seq, 0);
    return e;
}
static void RingPublish(Event* e, LONG64 idx) { InterlockedExchange64(&e->seq, idx + 1); }

// ----------------------------------------------------------------------------
// The device hooks
// ----------------------------------------------------------------------------
struct CallScope
{
    LONGLONG t0, w0;
    int      slot;
    CallScope() { slot = MySlot(); ++t_inCreate; w0 = t_waitTicks; t0 = Now(); }
};

static UINT64 ResourceBytes(ID3D12Device* d, const D3D12_RESOURCE_DESC* rd)
{
    if (!d || !rd) return 0;
    D3D12_RESOURCE_ALLOCATION_INFO ai = d->GetResourceAllocationInfo(0, 1, rd);
    return ai.SizeInBytes == UINT64_MAX ? 0 : ai.SizeInBytes;
}

// The cost is taken before anything here runs: sizing and recording are
// outside the timed window.
static void Finish(const CallScope& s, Kind k, HRESULT hr, UINT64 bytes,
                   const D3D12_HEAP_PROPERTIES* hp, UINT32 heapFlags,
                   const D3D12_RESOURCE_DESC* rd, UINT32 count)
{
    LONGLONG t1 = Now();
    LONGLONG dur = t1 - s.t0, wait = t_waitTicks - s.w0;
    --t_inCreate;

    Cat c = CatOf(k);
    InterlockedIncrement(&g_frame.count[c]);
    InterlockedAdd64(&g_frame.createTicks, dur);
    InterlockedAdd64(&g_frame.waitTicks, wait);
    if (SUCCEEDED(hr) && (c == C_COMMITTED || c == C_HEAP || c == C_SUB))
        InterlockedAdd64(&g_frame.bytes, (LONG64)bytes);
    ThreadSlot& ts = g_slots[s.slot];
    InterlockedAdd64(&ts.createTicks, dur);
    InterlockedAdd64(&ts.waitTicks, wait);
    InterlockedIncrement(&ts.creates);

    if (g_cfg.logging == 2 && g_ring)
    {
        LONG64 idx;
        Event* e = RingClaim(idx);
        e->qpc = s.t0; e->dur = dur; e->wait = wait; e->extra = 0;
        e->bytes = bytes; e->tid = ts.tid; e->slot = s.slot; e->kind = k; e->hr = hr;
        e->heapType = hp ? (unsigned short)hp->Type : 0;
        e->heapFlags = heapFlags; e->count = count;
        if (rd)
        {
            e->dim = (unsigned short)rd->Dimension; e->width = rd->Width; e->height = rd->Height;
            e->depth = rd->DepthOrArraySize; e->mips = rd->MipLevels; e->format = rd->Format;
            e->resFlags = rd->Flags;
        }
        else { e->dim = 0; e->width = 0; e->height = 0; e->depth = 0; e->mips = 0; e->format = 0; e->resFlags = 0; }
        RingPublish(e, idx);
    }
}

#define DESC(rd1) ((const D3D12_RESOURCE_DESC*)(rd1))   // DESC1 extends DESC; the prefix is the same

typedef HRESULT (STDMETHODCALLTYPE* PFN_CCR)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CCR1)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
    ID3D12ProtectedResourceSession*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CCR2)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
    ID3D12ProtectedResourceSession*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CCR3)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC1*, D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*,
    ID3D12ProtectedResourceSession*, UINT32, const DXGI_FORMAT*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CH)(ID3D12Device*, const D3D12_HEAP_DESC*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CH1)(ID3D12Device*, const D3D12_HEAP_DESC*,
    ID3D12ProtectedResourceSession*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CPR)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
    D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CPR1)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*,
    D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CPR2)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*,
    D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*, UINT32, const DXGI_FORMAT*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CRR)(ID3D12Device*, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
    const D3D12_CLEAR_VALUE*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CRR1)(ID3D12Device*, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
    const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CRR2)(ID3D12Device*, const D3D12_RESOURCE_DESC*, D3D12_BARRIER_LAYOUT,
    const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, UINT32, const DXGI_FORMAT*, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_RES)(ID3D12Device*, UINT, ID3D12Pageable* const*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ENQ)(ID3D12Device*, D3D12_RESIDENCY_FLAGS, UINT,
    ID3D12Pageable* const*, ID3D12Fence*, UINT64);

static PFN_CCR  o_CCR;  static PFN_CCR1 o_CCR1; static PFN_CCR2 o_CCR2; static PFN_CCR3 o_CCR3;
static PFN_CH   o_CH;   static PFN_CH1  o_CH1;
static PFN_CPR  o_CPR;  static PFN_CPR1 o_CPR1; static PFN_CPR2 o_CPR2;
static PFN_CRR  o_CRR;  static PFN_CRR1 o_CRR1; static PFN_CRR2 o_CRR2;
static PFN_RES  o_MakeResident, o_Evict;
static PFN_ENQ  o_Enqueue;

// ----------------------------------------------------------------------------
// Suballocation: 64 KB pages in large heaps, one bitmap per heap, first fit.
// Three pools, one per heap type the game makes small committed resources
// in: DEFAULT (GPU-only: textures and buffers), UPLOAD and READBACK (CPU-
// visible: buffers only, the only kind D3D12 allows there). Heaps are never
// released; each pool stops growing at its cap and falls back to committed.
// A grower thread keeps a block's worth of free space in every pool so a
// burst never waits for a new heap (0.2.0 added blocks mid-pass, at ~4 ms
// each); a pool that still runs dry makes one itself, outside the lock.
// ----------------------------------------------------------------------------
static const UINT64 SUB_PAGE = 65536;
static const int SUB_MAX_BLOCKS = 256;
struct SubPool;
struct SubBlock
{
    SubPool*    pool;
    ID3D12Heap* heap;
    UINT64*     bits;        // 1 = page in use
    UINT        pages, freePages;
};
enum PoolId { POOL_DEFAULT, POOL_UPLOAD, POOL_READBACK, POOL_COUNT };
struct SubPool
{
    const char*      name;
    D3D12_HEAP_TYPE  type;
    D3D12_HEAP_FLAGS heapFlags;
    UINT64           blockBytes, maxResource;
    int              maxBlocks, prewarm;
    SubBlock*        blocks[SUB_MAX_BLOCKS];
    volatile LONG    count;
    volatile LONG64  totalPages, freePages;
};
struct SubState
{
    SRWLOCK        lock = SRWLOCK_INIT;
    SubPool        pools[POOL_COUNT] = {};
    volatile LONG  on = 0;
    ID3D12Device*  dev = NULL;
    bool           tierOk = false;
    HANDLE         growEvent = NULL;
    volatile LONG  live = 0, made = 0, fellBack = 0, heapFails = 0, capped = 0;
};
static SubState g_sub;

// {7B3E6A51-2C4D-4F88-9A1E-5D2B8C0F4E17}: our tracker's private-data key
static const GUID SUB_TRACKER_GUID = { 0x7b3e6a51, 0x2c4d, 0x4f88, { 0x9a, 0x1e, 0x5d, 0x2b, 0x8c, 0x0f, 0x4e, 0x17 } };

struct SubSlot { SubBlock* block; UINT first, pages; };

static void SubSetupPools()
{
    struct { const char* name; D3D12_HEAP_TYPE type; D3D12_HEAP_FLAGS flags; int blockMB, maxMB, prewarm; } def[POOL_COUNT] = {
        { "DEFAULT",  D3D12_HEAP_TYPE_DEFAULT,  D3D12_HEAP_FLAG_CREATE_NOT_ZEROED,
          g_cfg.blockMB, g_cfg.maxPoolMB, g_cfg.prewarmBlocks },
        { "UPLOAD",   D3D12_HEAP_TYPE_UPLOAD,   D3D12_HEAP_FLAG_CREATE_NOT_ZEROED | D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS,
          g_cfg.hostBlockMB, g_cfg.maxHostPoolMB, 1 },
        { "READBACK", D3D12_HEAP_TYPE_READBACK, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED | D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS,
          g_cfg.hostBlockMB, g_cfg.maxHostPoolMB, 1 },
    };
    for (int i = 0; i < POOL_COUNT; ++i)
    {
        SubPool& p = g_sub.pools[i];
        p.name = def[i].name; p.type = def[i].type; p.heapFlags = def[i].flags;
        p.blockBytes = (UINT64)def[i].blockMB << 20;
        p.maxBlocks = def[i].maxMB / def[i].blockMB;
        if (p.maxBlocks > SUB_MAX_BLOCKS) p.maxBlocks = SUB_MAX_BLOCKS;
        p.prewarm = def[i].prewarm < p.maxBlocks ? def[i].prewarm : p.maxBlocks;
        // a resource never takes more than half a block, so one never strands most of one
        UINT64 cap = (UINT64)g_cfg.maxResourceKB * 1024;
        p.maxResource = cap < p.blockBytes / 2 ? cap : p.blockBytes / 2;
    }
}

static double PoolMB(LONG64 pages) { return pages * (double)SUB_PAGE / 1048576.0; }

// One line per pool, for STATS, the toggle and new blocks.
static void SubSummary(char* out, size_t n)
{
    int p = 0; out[0] = 0;
    for (int i = 0; i < POOL_COUNT; ++i)
    {
        const SubPool& s = g_sub.pools[i];
        p += _snprintf_s(out + p, n - p, _TRUNCATE, "%s%s %ld x %llu MB, %.1f MB used",
                         i ? " | " : "", s.name, s.count, s.blockBytes >> 20, PoolMB(s.totalPages - s.freePages));
    }
}

// Makes one heap outside the lock, then adds it under the lock.
static SubBlock* SubGrow(SubPool& p, const char* why)
{
    if (!o_CH || !g_sub.dev || p.count >= p.maxBlocks) return NULL;
    D3D12_HEAP_DESC hd = {};
    hd.SizeInBytes = p.blockBytes;
    hd.Properties.Type = p.type;
    hd.Flags = p.heapFlags;   // DEFAULT on tier 2: buffers and all textures together
    ID3D12Heap* heap = NULL;
    LONGLONG t0 = Now();
    if (FAILED(o_CH(g_sub.dev, &hd, IID_PPV_ARGS(&heap))) || !heap)
    {
        InterlockedIncrement(&g_sub.heapFails);
        Log("SUB: %s block FAILED to create (%llu MB) - that pool falls back to committed until one fits", p.name, p.blockBytes >> 20);
        return NULL;
    }
    double ms = Ms(Now() - t0);
    SubBlock* b = new SubBlock;
    b->pool = &p;
    b->heap = heap;
    b->pages = (UINT)(p.blockBytes / SUB_PAGE);
    b->freePages = b->pages;
    b->bits = new UINT64[(b->pages + 63) / 64]();
    AcquireSRWLockExclusive(&g_sub.lock);
    bool room = p.count < p.maxBlocks;
    if (room)
    {
        p.blocks[p.count] = b;
        InterlockedIncrement(&p.count);
        InterlockedAdd64(&p.totalPages, b->pages);
        InterlockedAdd64(&p.freePages, b->pages);
    }
    ReleaseSRWLockExclusive(&g_sub.lock);
    if (!room) { heap->Release(); delete[] b->bits; delete b; return NULL; }   // two growers raced past the cap
    if (why)
        Log("SUB: %s block %ld added (%s, %.1f ms) | %.0f MB pooled, %.1f MB used", p.name, p.count, why, ms,
            PoolMB(p.totalPages), PoolMB(p.totalPages - p.freePages));
    return b;
}

static int SubFindRun(const SubBlock* b, UINT n)
{
    for (UINT p = 0; p + n <= b->pages; )
    {
        if ((p & 63) == 0 && b->bits[p >> 6] == ~0ull) { p += 64; continue; }
        UINT i = 0;
        while (i < n && !((b->bits[(p + i) >> 6] >> ((p + i) & 63)) & 1)) ++i;
        if (i == n) return (int)p;
        p += i + 1;
    }
    return -1;
}
static void SubMark(SubBlock* b, UINT first, UINT n, bool used)
{
    for (UINT p = first; p < first + n; ++p)
    {
        UINT64 m = 1ull << (p & 63);
        if (used) b->bits[p >> 6] |= m; else b->bits[p >> 6] &= ~m;
    }
}

static bool SubAllocFrom(SubPool& p, UINT n, SubSlot& out)
{
    bool ok = false;
    AcquireSRWLockExclusive(&g_sub.lock);
    for (LONG i = 0; i < p.count && !ok; ++i)
    {
        SubBlock* b = p.blocks[i];
        if (b->freePages < n) continue;
        int r = SubFindRun(b, n);
        if (r < 0) continue;
        SubMark(b, (UINT)r, n, true);
        b->freePages -= n;
        InterlockedAdd64(&p.freePages, -(LONG64)n);
        out.block = b; out.first = (UINT)r; out.pages = n;
        ok = true;
    }
    ReleaseSRWLockExclusive(&g_sub.lock);
    return ok;
}

static bool SubAlloc(SubPool& p, UINT64 bytes, SubSlot& out)
{
    UINT n = (UINT)((bytes + SUB_PAGE - 1) / SUB_PAGE);
    bool ok = SubAllocFrom(p, n, out);
    if (!ok)
    {
        if (p.count >= p.maxBlocks) { InterlockedIncrement(&g_sub.capped); return false; }
        ok = SubGrow(p, "ran dry mid-burst") && SubAllocFrom(p, n, out);
    }
    if (p.freePages < (LONG64)(p.blockBytes / SUB_PAGE) && p.count < p.maxBlocks && g_sub.growEvent)
        SetEvent(g_sub.growEvent);   // keep a block's worth free ahead of the next burst
    return ok;
}
static void SubFree(const SubSlot& s)
{
    AcquireSRWLockExclusive(&g_sub.lock);
    SubMark(s.block, s.first, s.pages, false);
    s.block->freePages += s.pages;
    InterlockedAdd64(&s.block->pool->freePages, s.pages);
    ReleaseSRWLockExclusive(&g_sub.lock);
}

static DWORD WINAPI SubGrowThread(LPVOID)
{
    for (;;)
    {
        WaitForSingleObject(g_sub.growEvent, 1000);
        for (int i = 0; i < POOL_COUNT; ++i)
        {
            SubPool& p = g_sub.pools[i];
            while (g_sub.on && p.count > 0 && p.count < p.maxBlocks &&
                   p.freePages < (LONG64)(p.blockBytes / SUB_PAGE))
                if (!SubGrow(p, "ahead of need")) break;
        }
    }
}

// Held by the placed resource as private data; D3D12 releases it when it
// destroys the resource, and the pages go back to the pool then.
struct SubTracker final : IUnknown
{
    volatile LONG ref = 1;
    SubSlot       slot;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override
    {
        if (riid == __uuidof(IUnknown)) { *pp = this; AddRef(); return S_OK; }
        *pp = NULL; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&ref);
        if (r == 0)
        {
            SubFree(slot);
            InterlockedDecrement(&g_sub.live);
            delete this;
        }
        return (ULONG)r;
    }
};

// The shape the game's stutter is made of (see the header): the pool it
// goes to, or NULL to stay committed. DEFAULT takes buffers and textures
// (no render target or depth flags: placed ones of those need explicit
// initialization the game may not do); UPLOAD and READBACK take plain buffers.
static SubPool* SubEligible(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
                            const D3D12_RESOURCE_DESC* rd, const D3D12_CLEAR_VALUE* cv, void** pp,
                            UINT64& bytes)
{
    if (!g_sub.on || !g_sub.tierOk || !pp || !hp || !rd || cv) return NULL;
    if (hp->CreationNodeMask > 1 || hp->VisibleNodeMask > 1) return NULL;
    if (hf != D3D12_HEAP_FLAG_CREATE_NOT_ZEROED) return NULL;
    if (rd->SampleDesc.Count != 1 || rd->Dimension == D3D12_RESOURCE_DIMENSION_UNKNOWN) return NULL;
    SubPool* p = NULL;
    switch (hp->Type)
    {
    case D3D12_HEAP_TYPE_DEFAULT:
        if (rd->Flags & ~D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) return NULL;
        p = &g_sub.pools[POOL_DEFAULT];
        break;
    case D3D12_HEAP_TYPE_UPLOAD:
    case D3D12_HEAP_TYPE_READBACK:
        if (rd->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || rd->Flags) return NULL;
        p = &g_sub.pools[hp->Type == D3D12_HEAP_TYPE_UPLOAD ? POOL_UPLOAD : POOL_READBACK];
        break;
    default:
        return NULL;   // CUSTOM, GPU_UPLOAD: left as the game made them
    }
    if (p->maxBlocks <= 0) return NULL;
    D3D12_RESOURCE_ALLOCATION_INFO ai = d->GetResourceAllocationInfo(0, 1, rd);
    if (ai.SizeInBytes == UINT64_MAX || ai.SizeInBytes == 0) return NULL;
    if (ai.SizeInBytes > p->maxResource || ai.Alignment > SUB_PAGE) return NULL;
    bytes = ai.SizeInBytes;
    return p;
}

// The placed resource exists: tie its pages to its lifetime. On failure the
// resource is released before its pages are, so they are never reused live.
static bool SubAttach(void** pp, const SubSlot& slot)
{
    ID3D12Object* obj = NULL;
    SubTracker* t = new SubTracker;
    t->slot = slot;
    HRESULT hr = ((IUnknown*)*pp)->QueryInterface(IID_PPV_ARGS(&obj));
    if (SUCCEEDED(hr))
    {
        hr = obj->SetPrivateDataInterface(SUB_TRACKER_GUID, t);
        obj->Release();
    }
    if (FAILED(hr))
    {
        ((IUnknown*)*pp)->Release();
        *pp = NULL;
        t->Release();   // frees the pages, now that nothing uses them
        return false;
    }
    InterlockedIncrement(&g_sub.live);
    InterlockedIncrement(&g_sub.made);
    t->Release();       // the resource holds the only reference now
    return true;
}

// One attempt for every committed variant: place(heap, offset) does the
// variant's own placed call. False = take the committed path as usual.
template <class F>
static bool SubTry(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
                   const D3D12_RESOURCE_DESC* rd, const D3D12_CLEAR_VALUE* cv, void** pp,
                   UINT64& bytes, HRESULT& hr, F place)
{
    SubPool* pool = SubEligible(d, hp, hf, rd, cv, pp, bytes);
    if (!pool) return false;
    SubSlot slot;
    if (!SubAlloc(*pool, bytes, slot)) { InterlockedIncrement(&g_sub.fellBack); return false; }
    hr = place(slot.block->heap, (UINT64)slot.first * SUB_PAGE);
    if (FAILED(hr) || !*pp)
    {
        SubFree(slot);
        InterlockedIncrement(&g_sub.fellBack);
        return false;
    }
    if (!SubAttach(pp, slot)) { InterlockedIncrement(&g_sub.fellBack); return false; }
    return true;
}

static void SubPrewarm(ID3D12Device* dev)
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
    bool tier = SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o))) &&
                o.ResourceHeapTier >= D3D12_RESOURCE_HEAP_TIER_2;
    g_sub.dev = dev;   // the game's device: alive for the whole session
    g_sub.tierOk = tier;
    if (!tier)
    {
        Log("setup: suballocation UNAVAILABLE (resource heap tier %d; needs 2) - the game runs as shipped", (int)o.ResourceHeapTier);
        return;
    }
    SubSetupPools();
    LONGLONG t0 = Now();
    for (int i = 0; i < POOL_COUNT; ++i)
        for (int k = 0; k < g_sub.pools[i].prewarm; ++k)
            if (!SubGrow(g_sub.pools[i], NULL)) break;
    double ms = Ms(Now() - t0);
    g_sub.growEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE t = CreateThread(NULL, 0, SubGrowThread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    InterlockedExchange(&g_sub.on, g_cfg.suballocate);
    char sum[256];
    SubSummary(sum, sizeof(sum));
    Log("setup: suballocation %s | prewarmed in %.1f ms: %s | resources up to %d KB (host pools %d KB) | caps %d MB / %d MB",
        g_sub.on ? "ON" : "off (ToggleKey switches it)", ms, sum,
        (int)(g_sub.pools[POOL_DEFAULT].maxResource / 1024), (int)(g_sub.pools[POOL_UPLOAD].maxResource / 1024),
        g_cfg.maxPoolMB, g_cfg.maxHostPoolMB);
}

static HRESULT STDMETHODCALLTYPE h_CCR(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
    const D3D12_RESOURCE_DESC* rd, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** pp)
{
    CallScope s;
    UINT64 bytes = 0; HRESULT hr;
    if (o_CPR && SubTry(d, hp, hf, rd, cv, pp, bytes, hr,
            [&](ID3D12Heap* h, UINT64 off) { return o_CPR(d, h, off, rd, st, NULL, riid, pp); }))
    { Finish(s, K_SUB, hr, bytes, hp, hf, rd, 0); return hr; }
    hr = o_CCR(d, hp, hf, rd, st, cv, riid, pp);
    Finish(s, K_COMMITTED, hr, ResourceBytes(d, rd), hp, hf, rd, 0);
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_CCR1(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
    const D3D12_RESOURCE_DESC* rd, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv,
    ID3D12ProtectedResourceSession* ps, REFIID riid, void** pp)
{
    CallScope s;
    UINT64 bytes = 0; HRESULT hr;
    if (!ps && o_CPR && SubTry(d, hp, hf, rd, cv, pp, bytes, hr,
            [&](ID3D12Heap* h, UINT64 off) { return o_CPR(d, h, off, rd, st, NULL, riid, pp); }))
    { Finish(s, K_SUB1, hr, bytes, hp, hf, rd, 0); return hr; }
    hr = o_CCR1(d, hp, hf, rd, st, cv, ps, riid, pp);
    Finish(s, K_COMMITTED1, hr, ResourceBytes(d, rd), hp, hf, rd, 0);
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_CCR2(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
    const D3D12_RESOURCE_DESC1* rd, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv,
    ID3D12ProtectedResourceSession* ps, REFIID riid, void** pp)
{
    CallScope s;
    UINT64 bytes = 0; HRESULT hr;
    if (!ps && o_CPR1 && SubTry(d, hp, hf, DESC(rd), cv, pp, bytes, hr,
            [&](ID3D12Heap* h, UINT64 off) { return o_CPR1(d, h, off, rd, st, NULL, riid, pp); }))
    { Finish(s, K_SUB2, hr, bytes, hp, hf, DESC(rd), 0); return hr; }
    hr = o_CCR2(d, hp, hf, rd, st, cv, ps, riid, pp);
    Finish(s, K_COMMITTED2, hr, ResourceBytes(d, DESC(rd)), hp, hf, DESC(rd), 0);
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_CCR3(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
    const D3D12_RESOURCE_DESC1* rd, D3D12_BARRIER_LAYOUT lay, const D3D12_CLEAR_VALUE* cv,
    ID3D12ProtectedResourceSession* ps, UINT32 nf, const DXGI_FORMAT* fmts, REFIID riid, void** pp)
{
    CallScope s;
    UINT64 bytes = 0; HRESULT hr;
    if (!ps && o_CPR2 && SubTry(d, hp, hf, DESC(rd), cv, pp, bytes, hr,
            [&](ID3D12Heap* h, UINT64 off) { return o_CPR2(d, h, off, rd, lay, NULL, nf, fmts, riid, pp); }))
    { Finish(s, K_SUB3, hr, bytes, hp, hf, DESC(rd), 0); return hr; }
    hr = o_CCR3(d, hp, hf, rd, lay, cv, ps, nf, fmts, riid, pp);
    Finish(s, K_COMMITTED3, hr, ResourceBytes(d, DESC(rd)), hp, hf, DESC(rd), 0);
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_CH(ID3D12Device* d, const D3D12_HEAP_DESC* hd, REFIID riid, void** pp)
{
    CallScope s;
    HRESULT hr = o_CH(d, hd, riid, pp);
    Finish(s, K_HEAP, hr, hd ? hd->SizeInBytes : 0, hd ? &hd->Properties : NULL, hd ? hd->Flags : 0, NULL, 0);
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_CH1(ID3D12Device* d, const D3D12_HEAP_DESC* hd,
    ID3D12ProtectedResourceSession* ps, REFIID riid, void** pp)
{
    CallScope s;
    HRESULT hr = o_CH1(d, hd, ps, riid, pp);
    Finish(s, K_HEAP1, hr, hd ? hd->SizeInBytes : 0, hd ? &hd->Properties : NULL, hd ? hd->Flags : 0, NULL, 0);
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_CPR(ID3D12Device* d, ID3D12Heap* h, UINT64 off, const D3D12_RESOURCE_DESC* rd,
    D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** pp)
{
    CallScope s;
    HRESULT hr = o_CPR(d, h, off, rd, st, cv, riid, pp);
    Finish(s, K_PLACED, hr, ResourceBytes(d, rd), NULL, 0, rd, 0);
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_CPR1(ID3D12Device* d, ID3D12Heap* h, UINT64 off, const D3D12_RESOURCE_DESC1* rd,
    D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** pp)
{
    CallScope s;
    HRESULT hr = o_CPR1(d, h, off, rd, st, cv, riid, pp);
    Finish(s, K_PLACED1, hr, ResourceBytes(d, DESC(rd)), NULL, 0, DESC(rd), 0);
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_CPR2(ID3D12Device* d, ID3D12Heap* h, UINT64 off, const D3D12_RESOURCE_DESC1* rd,
    D3D12_BARRIER_LAYOUT lay, const D3D12_CLEAR_VALUE* cv, UINT32 nf, const DXGI_FORMAT* fmts, REFIID riid, void** pp)
{
    CallScope s;
    HRESULT hr = o_CPR2(d, h, off, rd, lay, cv, nf, fmts, riid, pp);
    Finish(s, K_PLACED2, hr, ResourceBytes(d, DESC(rd)), NULL, 0, DESC(rd), 0);
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_CRR(ID3D12Device* d, const D3D12_RESOURCE_DESC* rd, D3D12_RESOURCE_STATES st,
    const D3D12_CLEAR_VALUE* cv, REFIID riid, void** pp)
{
    CallScope s;
    HRESULT hr = o_CRR(d, rd, st, cv, riid, pp);
    Finish(s, K_RESERVED, hr, 0, NULL, 0, rd, 0);
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_CRR1(ID3D12Device* d, const D3D12_RESOURCE_DESC* rd, D3D12_RESOURCE_STATES st,
    const D3D12_CLEAR_VALUE* cv, ID3D12ProtectedResourceSession* ps, REFIID riid, void** pp)
{
    CallScope s;
    HRESULT hr = o_CRR1(d, rd, st, cv, ps, riid, pp);
    Finish(s, K_RESERVED1, hr, 0, NULL, 0, rd, 0);
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_CRR2(ID3D12Device* d, const D3D12_RESOURCE_DESC* rd, D3D12_BARRIER_LAYOUT lay,
    const D3D12_CLEAR_VALUE* cv, ID3D12ProtectedResourceSession* ps, UINT32 nf, const DXGI_FORMAT* fmts,
    REFIID riid, void** pp)
{
    CallScope s;
    HRESULT hr = o_CRR2(d, rd, lay, cv, ps, nf, fmts, riid, pp);
    Finish(s, K_RESERVED2, hr, 0, NULL, 0, rd, 0);
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_MakeResident(ID3D12Device* d, UINT n, ID3D12Pageable* const* objs)
{
    CallScope s;
    HRESULT hr = o_MakeResident(d, n, objs);
    Finish(s, K_MAKERESIDENT, hr, 0, NULL, 0, NULL, n);
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_Evict(ID3D12Device* d, UINT n, ID3D12Pageable* const* objs)
{
    CallScope s;
    HRESULT hr = o_Evict(d, n, objs);
    Finish(s, K_EVICT, hr, 0, NULL, 0, NULL, n);
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_Enqueue(ID3D12Device* d, D3D12_RESIDENCY_FLAGS f, UINT n,
    ID3D12Pageable* const* objs, ID3D12Fence* fence, UINT64 value)
{
    CallScope s;
    HRESULT hr = o_Enqueue(d, f, n, objs, fence, value);
    Finish(s, K_ENQUEUE, hr, 0, NULL, (UINT32)f, NULL, n);
    return hr;
}

// ----------------------------------------------------------------------------
// The kernel wait (MinHook): D3D12's residency fence wait goes through here
// ----------------------------------------------------------------------------
typedef LONG (APIENTRY* PFN_KMTWAIT)(const void*);
static PFN_KMTWAIT o_KmtWait;
static LONG APIENTRY h_KmtWait(const void* args)
{
    LONGLONG t0 = Now();
    LONG r = o_KmtWait(args);
    LONGLONG dt = Now() - t0;
    t_waitTicks += dt;
    if (!t_inCreate) InterlockedAdd64(&g_frame.otherWaitTicks, dt);
    return r;
}

// ----------------------------------------------------------------------------
// Vtable patching: the original is stored before the entry is swapped, so a
// call racing the patch reaches either the original or a ready detour.
// ----------------------------------------------------------------------------
static bool PatchSlot(void** vt, int idx, void* detour, void** orig)
{
    DWORD old;
    if (!VirtualProtect(&vt[idx], sizeof(void*), PAGE_READWRITE, &old)) return false;
    *orig = vt[idx];
    InterlockedExchangePointer(&vt[idx], detour);
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
    return true;
}

static void ModuleOf(const void* addr, char* out, size_t outSize, UINT_PTR* base)
{
    HMODULE m = NULL;
    out[0] = 0; *base = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &m) && m)
    {
        GetModuleFileNameA(m, out, (DWORD)outSize);
        *base = (UINT_PTR)m;
    }
}

static void FileVersion(const char* path, char* out, size_t outSize)
{
    _snprintf_s(out, outSize, _TRUNCATE, "?");
    DWORD h = 0, n = GetFileVersionInfoSizeA(path, &h);
    if (!n) return;
    void* buf = malloc(n);
    if (!buf) return;
    VS_FIXEDFILEINFO* fi = NULL; UINT len = 0;
    if (GetFileVersionInfoA(path, 0, n, buf) && VerQueryValueA(buf, "\\", (void**)&fi, &len) && fi)
        _snprintf_s(out, outSize, _TRUNCATE, "%u.%u.%u.%u",
                    HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                    HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    free(buf);
}

// ----------------------------------------------------------------------------
// Frames: Present / Present1 on the swap chain's vtable
// ----------------------------------------------------------------------------
typedef HRESULT (STDMETHODCALLTYPE* PFN_PRESENT)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* PFN_PRESENT1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static PFN_PRESENT  o_Present;
static PFN_PRESENT1 o_Present1;
static thread_local int t_inPresent = 0;

struct Stats
{
    LONGLONG windowStart = 0, last = 0;
    LONG     frames = 0, hitches = 0, creates = 0;
    double   frameMs = 0, worstMs = 0, mb = 0, createMs = 0, waitMs = 0;
};
static Stats g_stats;   // touched by the presenting thread only

static void OnFrame()
{
    LONGLONG now = Now();
    if (!g_stats.last)
    {
        g_stats.last = g_stats.windowStart = now;
        for (int c = 0; c < C_COUNT; ++c) InterlockedExchange(&g_frame.count[c], 0);
        InterlockedExchange64(&g_frame.createTicks, 0); InterlockedExchange64(&g_frame.waitTicks, 0);
        InterlockedExchange64(&g_frame.otherWaitTicks, 0); InterlockedExchange64(&g_frame.bytes, 0);
        return;
    }
    LONGLONG ft = now - g_stats.last;
    g_stats.last = now;

    LONG cnt[C_COUNT], creates = 0;
    for (int c = 0; c < C_COUNT; ++c) { cnt[c] = InterlockedExchange(&g_frame.count[c], 0); creates += cnt[c]; }
    LONGLONG ct = InterlockedExchange64(&g_frame.createTicks, 0);
    LONGLONG wt = InterlockedExchange64(&g_frame.waitTicks, 0);
    LONGLONG ot = InterlockedExchange64(&g_frame.otherWaitTicks, 0);
    LONGLONG by = InterlockedExchange64(&g_frame.bytes, 0);

    // the three threads that spent longest in create calls this frame
    struct Top { int slot; LONGLONG ticks, wait; LONG n; } top[3] = { {-1,0,0,0}, {-1,0,0,0}, {-1,0,0,0} };
    LONG ns = g_slotCount; if (ns > MAX_SLOTS) ns = MAX_SLOTS;
    for (int i = 0; i < ns; ++i)
    {
        LONGLONG t = InterlockedExchange64(&g_slots[i].createTicks, 0);
        LONGLONG w = InterlockedExchange64(&g_slots[i].waitTicks, 0);
        LONG n = InterlockedExchange(&g_slots[i].creates, 0);
        for (int k = 0; k < 3; ++k)
            if (t > top[k].ticks)
            {
                for (int j = 2; j > k; --j) top[j] = top[j - 1];
                top[k].slot = i; top[k].ticks = t; top[k].wait = w; top[k].n = n;
                break;
            }
    }

    double ms = Ms(ft);
    bool hitch = ms >= g_cfg.hitchMs;
    g_stats.frames++; g_stats.frameMs += ms; if (ms > g_stats.worstMs) g_stats.worstMs = ms;
    g_stats.creates += creates; g_stats.mb += by / 1048576.0;
    g_stats.createMs += Ms(ct); g_stats.waitMs += Ms(wt);
    if (hitch) g_stats.hitches++;

    if (g_cfg.logging == 2 && g_ring)
    {
        LONG64 idx;
        Event* e = RingClaim(idx);
        e->qpc = now - ft; e->dur = ft; e->wait = wt; e->extra = ot; e->bytes = (UINT64)by;
        e->width = (UINT64)ct; e->tid = GetCurrentThreadId(); e->slot = -1; e->kind = K_FRAME;
        e->count = (UINT32)creates; e->hr = S_OK;
        RingPublish(e, idx);
    }

    if (hitch && g_cfg.logging)
    {
        char tops[256]; int p = 0; tops[0] = 0;
        for (int k = 0; k < 3 && top[k].slot >= 0; ++k)
            p += _snprintf_s(tops + p, sizeof(tops) - p, _TRUNCATE, "%s%s %.1f ms/%ld (wait %.1f)",
                             k ? ", " : "", g_slots[top[k].slot].name, Ms(top[k].ticks), top[k].n, Ms(top[k].wait));
        Log("HITCH %.1f ms | creates %ld (committed %ld, sub %ld, heap %ld, placed %ld, reserved %ld, residency %ld) %.1f MB"
            " | thread-ms in creates %.1f (residency wait %.1f) | other KMT waits %.1f | fix %s | top: %s",
            ms, creates, cnt[C_COMMITTED], cnt[C_SUB], cnt[C_HEAP], cnt[C_PLACED], cnt[C_RESERVED], cnt[C_RESIDENCY],
            by / 1048576.0, Ms(ct), Ms(wt), Ms(ot), g_sub.on ? "on" : "off", tops[0] ? tops : "-");
    }

    if (Ms(now - g_stats.windowStart) >= 30000.0)
    {
        Log("STATS %.0f s: %ld frames, avg %.2f ms, worst %.1f ms, hitches %ld | creates %ld (%.1f/frame) %.1f MB"
            " | thread-ms in creates %.1f (residency wait %.1f)",
            Ms(now - g_stats.windowStart) / 1000.0, g_stats.frames, g_stats.frameMs / (g_stats.frames ? g_stats.frames : 1),
            g_stats.worstMs, g_stats.hitches, g_stats.creates,
            g_stats.creates / (double)(g_stats.frames ? g_stats.frames : 1), g_stats.mb, g_stats.createMs, g_stats.waitMs);
        if (g_sub.tierOk)
        {
            char sum[256];
            SubSummary(sum, sizeof(sum));
            Log("SUB fix %s | %s | %ld live, %ld made, %ld fell back (%ld at a pool cap), %ld block failures",
                g_sub.on ? "on" : "off", sum, g_sub.live, g_sub.made, g_sub.fellBack, g_sub.capped, g_sub.heapFails);
        }
        LONGLONG keepLast = g_stats.last;
        g_stats = Stats();
        g_stats.last = keepLast; g_stats.windowStart = now;
    }
}

static HRESULT STDMETHODCALLTYPE h_Present(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    if (!t_inPresent++ && !(flags & DXGI_PRESENT_TEST)) OnFrame();
    HRESULT hr = o_Present(sc, sync, flags);
    --t_inPresent;
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
{
    if (!t_inPresent++ && !(flags & DXGI_PRESENT_TEST)) OnFrame();
    HRESULT hr = o_Present1(sc, sync, flags, pp);
    --t_inPresent;
    return hr;
}

// Every swap chain of the same class shares one vtable, so a throwaway 8x8
// one on a hidden window shows where Present lives. The code there is
// hooked (MinHook), not the vtable slot: overlays hook Present too, and a
// slot two hooks keep re-patching can end up with each one's "original"
// pointing at the other - the 0.1.0 first build overflowed the stack on the
// game's first Present that way (10/18, Steam overlay loaded). Hooks on the
// code chain one after another whatever order they go in.
typedef HRESULT (WINAPI* PFN_CreateDXGIFactory2)(UINT, REFIID, void**);
static DWORD WINAPI PresentHookThread(LPVOID param)
{
    ID3D12Device* dev = (ID3D12Device*)param;
    const char* fail = NULL;
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = g_files.module;
    wc.lpszClassName = "ishimurapatch_probe";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "", WS_POPUP, 0, 0, 8, 8, NULL, NULL, g_files.module, NULL);
    ID3D12CommandQueue* q = NULL;
    IDXGIFactory2* fac = NULL;
    IDXGISwapChain1* sc = NULL;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    PFN_CreateDXGIFactory2 pCreate = (PFN_CreateDXGIFactory2)GetProcAddress(LoadLibraryA("dxgi.dll"), "CreateDXGIFactory2");
    if (!hwnd) fail = "probe window";
    else if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) fail = "probe command queue";
    else if (!pCreate || FAILED(pCreate(0, IID_PPV_ARGS(&fac)))) fail = "DXGI factory";
    else
    {
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = 8; sd.Height = 8; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (FAILED(fac->CreateSwapChainForHwnd(q, hwnd, &sd, NULL, NULL, &sc))) fail = "probe swap chain";
    }
    if (!fail)
    {
        void** vt = *(void***)sc;
        struct { int idx; void* det; void** orig; const char* name; } fns[] = {
            { VT_Present,  (void*)h_Present,  (void**)&o_Present,  "Present" },
            { VT_Present1, (void*)h_Present1, (void**)&o_Present1, "Present1" },
        };
        for (auto& f : fns)
        {
            void* target = vt[f.idx];
            MH_STATUS s = MH_CreateHook(target, f.det, f.orig);
            if (s == MH_OK) s = MH_EnableHook(target);
            char mod[MAX_PATH]; UINT_PTR base;
            ModuleOf(target, mod, sizeof(mod), &base);
            Log("setup: %s %s (code at %s+0x%llX%s)", f.name, s == MH_OK ? "hooked" : MH_StatusToString(s),
                strrchr(mod, '\\') ? strrchr(mod, '\\') + 1 : mod, (unsigned long long)((UINT_PTR)target - base),
                _stricmp(strrchr(mod, '\\') ? strrchr(mod, '\\') + 1 : mod, "dxgi.dll") ? "; the slot was already redirected" : "");
        }
    }
    else Log("setup: Present hook NOT INSTALLED (%s failed) - no frame timing; PresentMon still lines up by QPC", fail);
    SubPrewarm(dev);
    if (sc) sc->Release();
    if (fac) fac->Release();
    if (q) q->Release();
    if (hwnd) DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, g_files.module);
    dev->Release();
    return 0;
}

// ----------------------------------------------------------------------------
// [Engine] settings: once the game has its device (config parsed, settings
// objects built), each "Class.Field = value" is resolved (frostbite.h finds
// the live objects by shape) and written, retrying for two minutes while the
// objects appear. Then every 2 s each is checked and restored if the engine
// changed it back (logged; a setting restored 50 times is given up on).
// The list is live: a saved ini adds, changes or takes out settings within a
// second, and a setting taken out gets the game's own value back (each
// object's value from before our first write).
// Lift A/B, 10/18: with [Fix] on, ForceLoadStreamingImmediate = 0 took the
// worst frame per pass from 14.5-20.5 ms to 11.5-12.5 ms over five passes
// each; a 1 ms ForceLoadStreamingImmediateQuantumMs instead spiked to 22.9,
// and splitting mesh streaming work over 4 frames made no real difference.
// ----------------------------------------------------------------------------
static bool DeviceReady();   // the game's D3D12 device has been found (below)

// The [Engine] list lives in g_cfg; a live reload replaces it under this
// section and raises the flag, and EngineThread works out what changed.
static LazyCs        g_engineCs;
static volatile LONG g_engineDirty = 1;     // the boot list counts as a change
static HANDLE        g_engineWake = NULL;   // set by a reload, so it's seen at once

struct EngineSlot
{
    SettingRef r;
    bool resolved;
    int  tries, restored;
    int  addedTick;
};

static bool SameSetting(const SettingRef& r, const char* cls, const char* fld)
{
    return !strcmp(r.cls, cls) && !strcmp(r.fld, fld);
}

static DWORD WINAPI EngineThread(LPVOID)
{
    static EngineSlot slots[16], next[16];
    int n = 0;
    for (int w = 0; w < 600 && !DeviceReady(); ++w) Sleep(100);
    Sleep(5000);
    ImgInit();
    for (int tick = 0;; ++tick)
    {
        if (InterlockedExchange(&g_engineDirty, 0))
        {
            decltype(g_cfg.engine) want;
            EnsureCs(g_engineCs);
            EnterCriticalSection(&g_engineCs.cs);
            int nw = g_cfg.nEngine;
            memcpy(want, g_cfg.engine, sizeof(want));
            LeaveCriticalSection(&g_engineCs.cs);
            bool taken[16] = {};
            int nn = 0;
            for (int i = 0; i < nw; ++i)
            {
                EngineSlot& s = next[nn++];
                int j = 0;
                while (j < n && (taken[j] || !SameSetting(slots[j].r, want[i].cls, want[i].fld))) ++j;
                if (j < n)
                {
                    // kept: a new value is written at once; objects and originals stay
                    taken[j] = true;
                    s = slots[j];
                    if (strcmp(s.r.val, want[i].val))
                    {
                        strncpy_s(s.r.val, sizeof(s.r.val), want[i].val, _TRUNCATE);
                        s.restored = 0;
                        Log("ENGINE: %s.%s = %s (live)", s.r.cls, s.r.fld, s.r.val);
                        if (s.resolved && EncodeSetting(s.r)) ApplySetting(s.r, "ENGINE: set");
                    }
                    continue;
                }
                s = EngineSlot();
                strncpy_s(s.r.cls, sizeof(s.r.cls), want[i].cls, _TRUNCATE);
                strncpy_s(s.r.fld, sizeof(s.r.fld), want[i].fld, _TRUNCATE);
                strncpy_s(s.r.val, sizeof(s.r.val), want[i].val, _TRUNCATE);
                s.addedTick = tick;
                if (tick) Log("ENGINE: %s.%s = %s added (live)", s.r.cls, s.r.fld, s.r.val);
            }
            for (int j = 0; j < n; ++j)
            {
                if (taken[j]) continue;
                bool ours = false;
                for (int o = 0; o < slots[j].r.k; ++o) ours |= slots[j].r.haveOrig[o];
                Log("ENGINE: %s.%s taken out (live)%s", slots[j].r.cls, slots[j].r.fld,
                    ours ? " - the game's own value goes back" : " - it was never changed");
                if (ours) PutBackSetting(slots[j].r, "ENGINE: put back");
            }
            memcpy(slots, next, sizeof(slots));
            n = nn;
        }
        for (int i = 0; i < n; ++i)
        {
            EngineSlot& s = slots[i];
            int age = tick - s.addedTick;
            if (!s.resolved)
            {
                // objects can appear late: every 5 s for two minutes
                if (s.tries >= 24 || age % 5) continue;
                bool last = ++s.tries == 24;
                if (ResolveSetting(s.r, "ENGINE", !last))
                {
                    s.resolved = true;
                    if (ApplySetting(s.r, "ENGINE: set") == 0) Log("ENGINE: %s.%s already %s", s.r.cls, s.r.fld, s.r.val);
                }
                else if (last || !s.r.width)   // !width: no such class, field or writable type
                {
                    s.tries = 24;
                    Log("ENGINE: %s.%s NOT applied - the game keeps its own value", s.r.cls, s.r.fld);
                }
                continue;
            }
            // kept: the engine may write its own value back
            if (age % 2 || s.restored >= 50) continue;
            int done = ApplySetting(s.r, "ENGINE: restored");
            if (done < 0)
            {
                Log("ENGINE: %s.%s object no longer readable - finding it again", s.r.cls, s.r.fld);
                if (ResolveSetting(s.r, "ENGINE")) ApplySetting(s.r, "ENGINE: set");
                continue;
            }
            if (done > 0 && ++s.restored >= 50)
                Log("ENGINE: %s.%s keeps being reset by the game - no longer enforced", s.r.cls, s.r.fld);
        }
        WaitForSingleObject(g_engineWake, 1000);
    }
}

// ----------------------------------------------------------------------------
// The development probe: ishimurapatch_probe.dll beside the .asi holds
// reverse-engineering code that changes often (now: the settings inspector,
// see probe.cpp). This thread watches the file; each new version is copied
// to %TEMP%, loaded, run once (ProbeRun) and unloaded, so a probe runs in
// the live game without a restart. [Advanced] Probe = 1 turns it on.
// ----------------------------------------------------------------------------
struct ProbeApi
{
    void (*log)(const char* line);
    LONGLONG qpcFreq;
};
typedef void (*PFN_ProbeRun)(const ProbeApi*);
static void ProbeLog(const char* line) { Log("%s", line); }

static DWORD WINAPI ProbeWatchThread(LPVOID)
{
    char src[MAX_PATH];
    strncpy_s(src, sizeof(src), g_files.logPath, _TRUNCATE);
    char* slash = strrchr(src, '\\');
    if (!slash) return 0;
    strcpy_s(slash + 1, src + sizeof(src) - slash - 1, "ishimurapatch_probe.dll");
    Sleep(20000);   // past the boot-time config parse
    FILETIME last = {};
    int gen = 0;
    for (;; Sleep(1000))
    {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExA(src, GetFileExInfoStandard, &fa) || !CompareFileTime(&fa.ftLastWriteTime, &last)) continue;
        Sleep(500);   // let whoever is writing it finish
        if (!GetFileAttributesExA(src, GetFileExInfoStandard, &fa)) continue;
        last = fa.ftLastWriteTime;
        char dst[MAX_PATH];
        GetTempPathA(MAX_PATH, dst);
        char name[64];
        _snprintf_s(name, sizeof(name), _TRUNCATE, "ishimurapatch_probe_%lu_%d.dll", GetCurrentProcessId(), ++gen);
        strcat_s(dst, sizeof(dst), name);
        if (!CopyFileA(src, dst, FALSE)) { Log("PROBE: copy FAILED (%lu)", GetLastError()); continue; }
        HMODULE h = LoadLibraryA(dst);
        PFN_ProbeRun run = h ? (PFN_ProbeRun)GetProcAddress(h, "ProbeRun") : NULL;
        Log("PROBE: generation %d %s", gen, run ? "loaded, running" : "could not be loaded");
        LONGLONG t0 = Now();
        if (run)
        {
            ProbeApi api = { ProbeLog, g_qpcFreq };
            run(&api);
        }
        if (h) FreeLibrary(h);
        DeleteFileA(dst);
        Log("PROBE: generation %d finished in %.0f ms, unloaded", gen, Ms(Now() - t0));
    }
}

// ----------------------------------------------------------------------------
// The device: found when the game creates it, or created by us if the game
// beat the hook (D3D12 returns the one device per adapter either way).
// ----------------------------------------------------------------------------
typedef HRESULT (WINAPI* PFN_D3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
static PFN_D3D12CreateDevice o_D3D12CreateDevice;
static volatile LONG g_devicePatched = 0;
static bool DeviceReady() { return g_devicePatched != 0; }

static void PatchDevice(ID3D12Device* dev, const char* how)
{
    if (InterlockedCompareExchange(&g_devicePatched, 1, 0) != 0) return;
    void** vt = *(void***)dev;
    int level = 0;
    {
        IUnknown* u = NULL;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D12Device3), (void**)&u))) { level = 3; u->Release(); }
        if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D12Device4), (void**)&u))) { level = 4; u->Release(); }
        if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D12Device8), (void**)&u))) { level = 8; u->Release(); }
        if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D12Device10), (void**)&u))) { level = 10; u->Release(); }
    }
    char mod[MAX_PATH], ver[64]; UINT_PTR base;
    ModuleOf(vt, mod, sizeof(mod), &base);
    FileVersion(mod, ver, sizeof(ver));
    Log("setup: device found via %s | vtable %s+0x%llX (version %s) | interfaces up to ID3D12Device%d",
        how, mod, (unsigned long long)((UINT_PTR)vt - base), ver, level ? level : 1);

    struct { int idx; void* det; void** orig; int need; const char* name; } slots[] = {
        { VT_CreateCommittedResource,  (void*)h_CCR,  (void**)&o_CCR,  0,  "CreateCommittedResource" },
        { VT_CreateHeap,               (void*)h_CH,   (void**)&o_CH,   0,  "CreateHeap" },
        { VT_CreatePlacedResource,     (void*)h_CPR,  (void**)&o_CPR,  0,  "CreatePlacedResource" },
        { VT_CreateReservedResource,   (void*)h_CRR,  (void**)&o_CRR,  0,  "CreateReservedResource" },
        { VT_MakeResident,             (void*)h_MakeResident, (void**)&o_MakeResident, 0, "MakeResident" },
        { VT_Evict,                    (void*)h_Evict, (void**)&o_Evict, 0, "Evict" },
        { VT_EnqueueMakeResident,      (void*)h_Enqueue, (void**)&o_Enqueue, 3, "EnqueueMakeResident" },
        { VT_CreateCommittedResource1, (void*)h_CCR1, (void**)&o_CCR1, 4,  "CreateCommittedResource1" },
        { VT_CreateHeap1,              (void*)h_CH1,  (void**)&o_CH1,  4,  "CreateHeap1" },
        { VT_CreateReservedResource1,  (void*)h_CRR1, (void**)&o_CRR1, 4,  "CreateReservedResource1" },
        { VT_CreateCommittedResource2, (void*)h_CCR2, (void**)&o_CCR2, 8,  "CreateCommittedResource2" },
        { VT_CreatePlacedResource1,    (void*)h_CPR1, (void**)&o_CPR1, 8,  "CreatePlacedResource1" },
        { VT_CreateCommittedResource3, (void*)h_CCR3, (void**)&o_CCR3, 10, "CreateCommittedResource3" },
        { VT_CreatePlacedResource2,    (void*)h_CPR2, (void**)&o_CPR2, 10, "CreatePlacedResource2" },
        { VT_CreateReservedResource2,  (void*)h_CRR2, (void**)&o_CRR2, 10, "CreateReservedResource2" },
    };
    char okList[1024] = {}, skipList[512] = {};
    for (auto& s : slots)
    {
        bool have = s.need == 0 || level >= s.need;
        char* list = have ? okList : skipList;
        size_t size = have ? sizeof(okList) : sizeof(skipList);
        bool ok = have && PatchSlot(vt, s.idx, s.det, s.orig);
        if (have && !ok) list = skipList, size = sizeof(skipList);
        strcat_s(list, size, list[0] ? ", " : "");
        strcat_s(list, size, s.name);
        if (have && !ok) strcat_s(list, size, " (patch FAILED)");
    }
    Log("setup: hooked %s", okList);
    if (skipList[0]) Log("setup: not hooked (interface not offered) %s", skipList);

    dev->AddRef();
    HANDLE t = CreateThread(NULL, 0, PresentHookThread, dev, 0, NULL);
    if (t) CloseHandle(t); else dev->Release();
}

static HRESULT WINAPI h_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void** pp)
{
    HRESULT hr = o_D3D12CreateDevice(adapter, fl, riid, pp);
    if (SUCCEEDED(hr) && pp && *pp && !g_devicePatched)
    {
        ID3D12Device* dev = NULL;
        if (SUCCEEDED(((IUnknown*)*pp)->QueryInterface(IID_PPV_ARGS(&dev))))
        {
            PatchDevice(dev, "the game's D3D12CreateDevice");
            dev->Release();
        }
    }
    return hr;
}

// If the game made its device before the hook went in, take the same
// device ourselves after a grace period (D3D12 hands back the existing one).
static DWORD WINAPI FallbackThread(LPVOID)
{
    for (int i = 0; i < 600 && !g_devicePatched; ++i) Sleep(100);
    if (g_devicePatched || !o_D3D12CreateDevice || !GetModuleHandleA("D3D12Core.dll")) return 0;
    ID3D12Device* dev = NULL;
    if (SUCCEEDED(o_D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))) && dev)
    {
        PatchDevice(dev, "fallback (the game's device existed before the hook)");
        dev->Release();
    }
    else Log("setup: fallback device NOT FOUND - no D3D12 timing this session");
    return 0;
}

// ----------------------------------------------------------------------------
// The writer (Logging 2): drains the ring into the log every 100 ms
// ----------------------------------------------------------------------------
static const char* HeapTypeName(unsigned t)
{
    switch (t)
    {
    case D3D12_HEAP_TYPE_DEFAULT:  return "DEFAULT";
    case D3D12_HEAP_TYPE_UPLOAD:   return "UPLOAD";
    case D3D12_HEAP_TYPE_READBACK: return "READBACK";
    case D3D12_HEAP_TYPE_CUSTOM:   return "CUSTOM";
    case 5:                        return "GPU_UPLOAD";
    default:                       return "-";
    }
}
static const char* DimName(unsigned d)
{
    switch (d)
    {
    case D3D12_RESOURCE_DIMENSION_BUFFER:    return "buf";
    case D3D12_RESOURCE_DIMENSION_TEXTURE1D: return "tex1d";
    case D3D12_RESOURCE_DIMENSION_TEXTURE2D: return "tex2d";
    case D3D12_RESOURCE_DIMENSION_TEXTURE3D: return "tex3d";
    default:                                 return "-";
    }
}

static void WriteEvent(const Event& e)
{
    const char* who = (e.slot >= 0 && e.slot < MAX_SLOTS) ? g_slots[e.slot].name : "?";
    char fail[32] = "";
    if (FAILED(e.hr)) _snprintf_s(fail, sizeof(fail), _TRUNCATE, " FAILED 0x%08lX", (unsigned long)e.hr);
    switch (CatOf((Kind)e.kind))
    {
    case C_COMMITTED:
    case C_SUB:
    case C_PLACED:
    case C_RESERVED:
        LogAt(e.qpc, "CR %-10s %-26s %-8s hf=0x%03X | %-5s %llux%ux%u mips %u fmt %u rf=0x%02X | %9.1f KB | %.3f ms (wait %.3f)%s",
              KIND_NAME[e.kind], who, HeapTypeName(e.heapType), e.heapFlags, DimName(e.dim),
              (unsigned long long)e.width, e.height, e.depth, e.mips, e.format, e.resFlags,
              e.bytes / 1024.0, Ms(e.dur), Ms(e.wait), fail);
        return;
    case C_HEAP:
        LogAt(e.qpc, "HP %-10s %-26s %-8s hf=0x%03X | %9.1f KB | %.3f ms (wait %.3f)%s",
              KIND_NAME[e.kind], who, HeapTypeName(e.heapType), e.heapFlags, e.bytes / 1024.0,
              Ms(e.dur), Ms(e.wait), fail);
        return;
    default: break;
    }
    if (e.kind == K_FRAME)
        LogAt(e.qpc, "FRAME %.2f ms | creates %u %.1f MB | thread-ms in creates %.2f (wait %.2f) | other KMT waits %.2f",
              Ms(e.dur), e.count, e.bytes / 1048576.0, Ms((LONGLONG)e.width), Ms(e.wait), Ms(e.extra));
    else
        LogAt(e.qpc, "RS %-19s %-26s n=%u | %.3f ms (wait %.3f)%s",
              KIND_NAME[e.kind], who, e.count, Ms(e.dur), Ms(e.wait), fail);
}

static DWORD WINAPI WriterThread(LPVOID)
{
    LONG64 cursor = 0, dropped = 0;
    for (;;)
    {
        Sleep(100);
        LONG64 head = g_ringHead;
        if (head - cursor > RING_SIZE) { dropped += head - RING_SIZE - cursor; cursor = head - RING_SIZE; }
        while (cursor < head)
        {
            Event* slot = &g_ring[cursor & (RING_SIZE - 1)];
            LONG64 s = slot->seq;
            if (s != cursor + 1)
            {
                if (s > cursor + 1 || s == 0 && g_ringHead - cursor > RING_SIZE) { ++dropped; ++cursor; continue; }
                break;   // claimed, not yet published: next pass
            }
            Event copy = *slot;
            if (slot->seq != cursor + 1) { ++dropped; ++cursor; continue; }   // lapped while copying
            WriteEvent(copy);
            ++cursor;
        }
        if (dropped) { Log("ring: %lld events dropped (the writer fell behind)", dropped); dropped = 0; }
    }
}

// ----------------------------------------------------------------------------
// The capture key (Logging 2): the Mouse Fix capture, by ms stamps
// ----------------------------------------------------------------------------
static const double CAPTURE_WINDOW_MS = 60000.0;
static volatile LONG g_captureBusy = 0;

static WORD KeyVk(const char* k)
{
    if (!k || !k[0]) return 0;
    if ((k[0] == 'F' || k[0] == 'f') && k[1] >= '1' && k[1] <= '9')
    {
        int n = atoi(k + 1);
        if (n >= 1 && n <= 12) return (WORD)(VK_F1 + n - 1);
    }
    if (!k[1] && ((k[0] >= 'A' && k[0] <= 'Z') || (k[0] >= '0' && k[0] <= '9'))) return (WORD)k[0];
    if (!k[1] && k[0] >= 'a' && k[0] <= 'z') return (WORD)(k[0] - 32);
    return 0;
}
static bool GameHasFocus()
{
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

static DWORD WINAPI CaptureLog(LPVOID param)
{
    double stampNow = *(double*)&param;
    FILE* in = _fsopen(g_files.logPath, "rb", _SH_DENYNO);
    if (!in) { InterlockedExchange(&g_captureBusy, 0); return 0; }
    fseek(in, 0, SEEK_END);
    long len = ftell(in);
    fseek(in, 0, SEEK_SET);
    char* buf = (len > 0) ? (char*)malloc((size_t)len + 1) : NULL;
    size_t got = buf ? fread(buf, 1, (size_t)len, in) : 0;
    fclose(in);
    if (!buf) { InterlockedExchange(&g_captureBusy, 0); return 0; }
    buf[got] = 0;
    SYSTEMTIME st; GetLocalTime(&st);
    char path[MAX_PATH];
    strncpy_s(path, sizeof(path), g_files.logPath, _TRUNCATE);
    char* dot = strrchr(path, '.');
    if (!dot) { free(buf); InterlockedExchange(&g_captureBusy, 0); return 0; }
    _snprintf_s(dot, path + sizeof(path) - dot, _TRUNCATE, "_capture_%04d%02d%02d_%02d%02d%02d.log",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    FILE* out = NULL;
    if (fopen_s(&out, path, "wb") != 0 || !out) { free(buf); Log("CAPTURE: could not write %s", path); InterlockedExchange(&g_captureBusy, 0); return 0; }
    double from = stampNow - CAPTURE_WINDOW_MS;
    bool header = true, said = false;
    for (char* line = buf; *line; )
    {
        char* end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line + 1) : strlen(line);
        // The searches stop at this line's end: unbounded, each one ran to
        // the end of the whole log, and a capture of an 8 MB log never
        // finished (0.1.1 in-game, 10/18).
        if (end) *end = 0;
        // a stamped line reads "%10.3f| "; setup lines are kept whatever their stamp
        char* bar = (char*)memchr(line, '|', n < 16 ? n : 16);
        double stamp = bar ? atof(line) : -1.0;
        bool setup = strstr(line, "| setup:") != NULL;
        bool done = header && strstr(line, "--- ishimurapatch init done ---") != NULL;
        if (end) *end = '\n';
        bool inWindow = stamp >= from;
        if (!header && inWindow && !said) { fputs("... (the log up to the last minute left out, setup lines kept)\r\n", out); said = true; }
        if (header || inWindow || setup) fwrite(line, 1, n, out);
        if (done) header = false;
        line += n;
    }
    fclose(out);
    free(buf);
    Log("CAPTURE: saved to %s", path);
    InterlockedExchange(&g_captureBusy, 0);
    return 0;
}

// The capture key (Logging 2) and the fix's toggle key (any level). Both are
// looked up again each poll, so a live reload changes them.
static DWORD WINAPI KeyThread(LPVOID)
{
    bool was = false, tWas = false;
    for (;;)
    {
        Sleep(30);
        WORD vk = (g_cfg.logging == 2 && g_ring) ? KeyVk(g_cfg.captureKey) : 0;
        WORD tk = KeyVk(g_cfg.toggleKey);
        bool tDown = tk && (GetAsyncKeyState(tk) & 0x8000) != 0;
        if (tDown && !tWas && GameHasFocus())
        {
            if (!g_sub.tierOk) Log("FIX: suballocation can't be switched on (not available on this GPU, or the device isn't found yet)");
            else
            {
                LONG now = !g_sub.on;
                InterlockedExchange(&g_sub.on, now);
                char sum[256];
                SubSummary(sum, sizeof(sum));
                Log("FIX: suballocation switched %s by the player | %s | %ld live, %ld made, %ld fell back",
                    now ? "ON" : "OFF", sum, g_sub.live, g_sub.made, g_sub.fellBack);
                if (now && g_sub.growEvent) SetEvent(g_sub.growEvent);
            }
        }
        tWas = tDown;
        bool down = vk && (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !was && GameHasFocus() && InterlockedCompareExchange(&g_captureBusy, 1, 0) == 0)
        {
            Sleep(250);   // let the writer drain the frames up to the key press
            double stampNow = Ms(Now() - g_qpc0);
            Log("CAPTURE: marked by the player (the last %.0f s saved)", CAPTURE_WINDOW_MS / 1000.0);
            void* param = NULL;
            memcpy(&param, &stampNow, sizeof(stampNow));
            HANDLE t = CreateThread(NULL, 0, CaptureLog, param, 0, NULL);
            if (t) CloseHandle(t);
            else InterlockedExchange(&g_captureBusy, 0);
        }
        was = down;
    }
}

// ----------------------------------------------------------------------------
// LIVE CONFIG RELOAD (the Mouse Fix design). Live: Suballocate, ToggleKey,
// [Engine], Logging 0/1, HitchMs, CaptureKey. Read at launch (a change is
// logged as waiting for the next launch): the pool sizes, which are made
// once the device exists, Logging 2, whose ring is allocated at init, and
// Probe, a thread started at init.
// ----------------------------------------------------------------------------
static void LoadConfigLive()
{
    Config c;
    {   // the same reader as boot; only Logging keeps its last value
        Config fb; fb.logging = g_cfg.logging;
        ReadConfig(c, fb);
    }
    if (c.logging == 2 && !g_ring) { if (g_cfg.logging != 2) Log("config: Logging = 2 takes effect next launch"); c.logging = g_cfg.logging; }
    if (c.logging != g_cfg.logging)
    {
        if (!c.logging) Log("config: Logging = 0 (live) - the log stops here");
        g_cfg.logging = c.logging;
        Log("config: Logging = %d (live)", g_cfg.logging);
    }
    if (c.hitchMs != g_cfg.hitchMs) { g_cfg.hitchMs = c.hitchMs; Log("config: HitchMs = %d (live)", g_cfg.hitchMs); }
    if (strcmp(c.captureKey, g_cfg.captureKey))
    {
        strcpy_s(g_cfg.captureKey, sizeof(g_cfg.captureKey), c.captureKey);
        Log("config: CaptureKey = %s (live)", g_cfg.captureKey[0] ? g_cfg.captureKey : "(off)");
    }
    if (strcmp(c.toggleKey, g_cfg.toggleKey))
    {
        strcpy_s(g_cfg.toggleKey, sizeof(g_cfg.toggleKey), c.toggleKey);
        Log("config: ToggleKey = %s (live)", g_cfg.toggleKey[0] ? g_cfg.toggleKey : "(off)");
    }
    // Only a change in the file switches the fix, so an unrelated save
    // doesn't undo the toggle key.
    if (c.suballocate != g_cfg.suballocate)
    {
        g_cfg.suballocate = c.suballocate;
        if (!g_sub.tierOk) Log("FIX: Suballocate = %d (live) - applied once the device is set up", c.suballocate);
        else
        {
            InterlockedExchange(&g_sub.on, c.suballocate);
            char sum[256];
            SubSummary(sum, sizeof(sum));
            Log("FIX: suballocation switched %s (live) | %s | %ld live, %ld made, %ld fell back",
                c.suballocate ? "ON" : "OFF", sum, g_sub.live, g_sub.made, g_sub.fellBack);
            if (c.suballocate && g_sub.growEvent) SetEvent(g_sub.growEvent);
        }
    }
    {
        char later[160] = {};
        auto wait = [&](bool changed, const char* key)
        {
            if (!changed) return;
            if (later[0]) strcat_s(later, sizeof(later), ", ");
            strcat_s(later, sizeof(later), key);
        };
        wait(c.blockMB != g_cfg.blockMB, "BlockMB");
        wait(c.prewarmBlocks != g_cfg.prewarmBlocks, "PrewarmBlocks");
        wait(c.maxPoolMB != g_cfg.maxPoolMB, "MaxPoolMB");
        wait(c.maxResourceKB != g_cfg.maxResourceKB, "MaxResourceKB");
        wait(c.hostBlockMB != g_cfg.hostBlockMB, "HostBlockMB");
        wait(c.maxHostPoolMB != g_cfg.maxHostPoolMB, "MaxHostPoolMB");
        wait(c.probe != g_cfg.probe, "Probe");
        if (later[0]) Log("config: %s changed - read at launch, so it takes effect next launch", later);
    }
    bool engine = c.nEngine != g_cfg.nEngine;
    for (int i = 0; i < c.nEngine && !engine; ++i)
        engine = strcmp(c.engine[i].cls, g_cfg.engine[i].cls) || strcmp(c.engine[i].fld, g_cfg.engine[i].fld) ||
                 strcmp(c.engine[i].val, g_cfg.engine[i].val);
    if (engine)
    {
        EnsureCs(g_engineCs);
        EnterCriticalSection(&g_engineCs.cs);
        memcpy(g_cfg.engine, c.engine, sizeof(g_cfg.engine));
        g_cfg.nEngine = c.nEngine;
        LeaveCriticalSection(&g_engineCs.cs);
        InterlockedExchange(&g_engineDirty, 1);
        if (g_engineWake) SetEvent(g_engineWake);
    }
    Log("config reloaded");
}

// Watches the ini's timestamp; a save in any editor triggers a live reload
// within about a second. Debounced on a stable timestamp (truncate-then-
// write editors update it before the content lands), and a file too small
// to hold a setting is skipped, warned once per timestamp.
static DWORD WINAPI IniWatchThread(LPVOID)
{
    FILETIME last = { 0, 0 }, pend = { 0, 0 };
    int stable = 0;
    for (;;)
    {
        Sleep(500);
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (!GetFileAttributesExA(g_files.iniPath, GetFileExInfoStandard, &fad))
            continue;
        if (!last.dwLowDateTime && !last.dwHighDateTime)
        { last = fad.ftLastWriteTime; continue; }
        if (CompareFileTime(&fad.ftLastWriteTime, &last) == 0)
        { stable = 0; continue; }
        if (CompareFileTime(&fad.ftLastWriteTime, &pend) != 0)
        { pend = fad.ftLastWriteTime; stable = 0; continue; }
        if (++stable < 2) continue;
        if (fad.nFileSizeHigh == 0 && fad.nFileSizeLow < 8)
        {
            static FILETIME lastWarn = { 0, 0 };
            if (CompareFileTime(&fad.ftLastWriteTime, &lastWarn) != 0)
            {
                Log("ini reload skipped - file looks incomplete");
                lastWarn = fad.ftLastWriteTime;
            }
            continue;
        }
        last = fad.ftLastWriteTime;
        stable = 0;
        LoadConfigLive();
    }
}

// ----------------------------------------------------------------------------
// Init
// ----------------------------------------------------------------------------
static bool HookExport(const char* dll, const char* fn, void* detour, void** orig, const char* label)
{
    HMODULE m = LoadLibraryA(dll);
    void* target = m ? (void*)GetProcAddress(m, fn) : NULL;
    if (!target) { Log("%s NOT FOUND (%s!%s)", label, dll, fn); return false; }
    MH_STATUS s = MH_CreateHook(target, detour, orig);
    if (s == MH_OK) s = MH_EnableHook(target);
    char mod[MAX_PATH]; UINT_PTR base;
    ModuleOf(target, mod, sizeof(mod), &base);
    Log("%s %s (%s+0x%llX)", label, s == MH_OK ? "hooked" : MH_StatusToString(s),
        strrchr(mod, '\\') ? strrchr(mod, '\\') + 1 : mod, (unsigned long long)((UINT_PTR)target - base));
    return s == MH_OK;
}

static DWORD WINAPI InitThread(LPVOID)
{
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpcFreq = f.QuadPart;
    g_qpc0 = Now();
    LoadConfig();
    p_GetThreadDescription = (PFN_GetThreadDescription)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");

    time_t now = time(NULL);
    char ts[64] = {0};
    ctime_s(ts, sizeof(ts), &now);
    if (ts[0]) { size_t n = strlen(ts); if (n && ts[n - 1] == '\n') ts[n - 1] = '\0'; }
    Log("--- Ishimura Patch v%s --- %s", MOD_VERSION, ts);
    Log(g_files.iniCreated ? "ishimurapatch.ini not found - created with defaults"
                           : "config loaded from ishimurapatch.ini");
    Log("config: Logging = %d | HitchMs = %d | CaptureKey = %s", g_cfg.logging, g_cfg.hitchMs,
        g_cfg.captureKey[0] ? g_cfg.captureKey : "(off)");
    Log("config: Suballocate = %d | ToggleKey = %s | BlockMB = %d | PrewarmBlocks = %d | MaxPoolMB = %d | MaxResourceKB = %d"
        " | HostBlockMB = %d | MaxHostPoolMB = %d",
        g_cfg.suballocate, g_cfg.toggleKey[0] ? g_cfg.toggleKey : "(off)", g_cfg.blockMB, g_cfg.prewarmBlocks,
        g_cfg.maxPoolMB, g_cfg.maxResourceKB, g_cfg.hostBlockMB, g_cfg.maxHostPoolMB);
    Log("config: Probe = %d | [Engine] %d setting%s", g_cfg.probe, g_cfg.nEngine, g_cfg.nEngine == 1 ? "" : "s");
    for (int i = 0; i < g_cfg.nEngine; ++i)
        Log("config:   %s.%s = %s", g_cfg.engine[i].cls, g_cfg.engine[i].fld, g_cfg.engine[i].val);
    {
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        WIN32_FILE_ATTRIBUTE_DATA fa = {};
        GetFileAttributesExA(exe, GetFileExInfoStandard, &fa);
        Log("game: %s (%llu bytes)", exe, ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
    }
    Log("clock: QPC frequency %lld, qpc0 %lld (stamps are ms since qpc0; PresentMon's TimeInQPC is the same clock)",
        g_qpcFreq, g_qpc0);

    if (g_cfg.logging == 2)
    {
        g_ring = (Event*)VirtualAlloc(NULL, sizeof(Event) * RING_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_ring) Log("ring: allocation FAILED - per-request lines off this session");
    }

    MH_STATUS mh = MH_Initialize();
    if (mh != MH_OK) Log("MinHook init FAILED (%s) - nothing hooked", MH_StatusToString(mh));
    else
    {
        HookExport("gdi32.dll", "D3DKMTWaitForSynchronizationObjectFromCpu", (void*)h_KmtWait,
                   (void**)&o_KmtWait, "residency wait (D3DKMTWaitForSynchronizationObjectFromCpu)");
        if (GetModuleHandleA("D3D12Core.dll"))
            Log("note: D3D12Core.dll already loaded at init - the device may predate the hook (fallback armed)");
        HookExport("d3d12.dll", "D3D12CreateDevice", (void*)h_D3D12CreateDevice,
                   (void**)&o_D3D12CreateDevice, "D3D12CreateDevice");
    }

    Log("--- ishimurapatch init done ---");

    HANDLE t;
    if ((t = CreateThread(NULL, 0, FallbackThread, NULL, 0, NULL))) CloseHandle(t);
    if (g_cfg.logging == 2 && g_ring)
        if ((t = CreateThread(NULL, 0, WriterThread, NULL, 0, NULL))) CloseHandle(t);
    if ((t = CreateThread(NULL, 0, KeyThread, NULL, 0, NULL))) CloseHandle(t);
    if (g_cfg.probe && (t = CreateThread(NULL, 0, ProbeWatchThread, NULL, 0, NULL))) CloseHandle(t);
    g_engineWake = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (g_engineWake && (t = CreateThread(NULL, 0, EngineThread, NULL, 0, NULL))) CloseHandle(t);
    if ((t = CreateThread(NULL, 0, IniWatchThread, NULL, 0, NULL))) CloseHandle(t);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        // Nothing but thread creation under the loader lock (the Mouse Fix
        // rule): config I/O, LoadLibrary and hooking all run in InitThread.
        DisableThreadLibraryCalls(h);
        g_files.module = h;
        // vtable detours and resident threads: this module must never unload
        HMODULE pin = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (LPCSTR)&g_files.module, &pin);
        HANDLE t = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
