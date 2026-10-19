// ============================================================================
//  Ishimura Patch  (ishimurapatch.asi)  -  Dead Space (2023)
// ----------------------------------------------------------------------------
//  0.1.0 is a measurement build: it changes nothing the game does. It times
//  the game's D3D12 GPU memory requests and the residency waits inside them,
//  and logs the frames they slow down.
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

static const char* MOD_VERSION = "0.1.2";

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
};
static Config g_cfg;

static const char* DEFAULT_INI =
"; Ishimura Patch - Dead Space (2023)\n"
";\n"
"; 0.1.0 is a measurement build: it changes nothing in the game. It times\n"
"; the game's D3D12 GPU memory requests and logs the frames they slow down.\n"
"; Delete this file and relaunch to get the defaults back.\n"
"\n"
"[Logging]\n"
"; Write a log to ishimurapatch.log (a fresh file every launch; a session's\n"
"; log stops at 256 MB)\n"
";  0 = off\n"
";  1 = on (default): what was hooked, one HITCH line for every frame at\n"
";      least HitchMs long, and a STATS line every 30 seconds\n"
";  2 = verbose: also every GPU memory request and every frame, stamped in\n"
";      ms since the game started. Roughly 10-15 MB a minute; only worth\n"
";      turning on for a test run.\n"
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
"CaptureKey = F8\n";

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
    Config fb;
    g_cfg.logging = GetPrivateProfileIntA("Logging", "Logging", fb.logging, g_files.iniPath);
    g_cfg.hitchMs = GetPrivateProfileIntA("Logging", "HitchMs", fb.hitchMs, g_files.iniPath);
    GetPrivateProfileStringA("Logging", "CaptureKey", fb.captureKey, g_cfg.captureKey,
                             sizeof(g_cfg.captureKey), g_files.iniPath);
    if (g_cfg.logging < 0 || g_cfg.logging > 2) g_cfg.logging = 1;
    if (g_cfg.hitchMs < 1) g_cfg.hitchMs = fb.hitchMs;
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
    K_MAKERESIDENT, K_EVICT, K_ENQUEUE, K_FRAME, K_COUNT
};
static const char* KIND_NAME[K_COUNT] = {
    "committed", "committed1", "committed2", "committed3", "heap", "heap1",
    "placed", "placed1", "placed2", "reserved", "reserved1", "reserved2",
    "MakeResident", "Evict", "EnqueueMakeResident", "FRAME" };
enum Cat { C_COMMITTED, C_HEAP, C_PLACED, C_RESERVED, C_RESIDENCY, C_COUNT };
static Cat CatOf(Kind k)
{
    if (k <= K_COMMITTED3) return C_COMMITTED;
    if (k <= K_HEAP1) return C_HEAP;
    if (k <= K_PLACED2) return C_PLACED;
    if (k <= K_RESERVED2) return C_RESERVED;
    return C_RESIDENCY;
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
    if (SUCCEEDED(hr) && c != C_PLACED && c != C_RESERVED && c != C_RESIDENCY)
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

static HRESULT STDMETHODCALLTYPE h_CCR(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
    const D3D12_RESOURCE_DESC* rd, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** pp)
{
    CallScope s;
    HRESULT hr = o_CCR(d, hp, hf, rd, st, cv, riid, pp);
    Finish(s, K_COMMITTED, hr, ResourceBytes(d, rd), hp, hf, rd, 0);
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_CCR1(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
    const D3D12_RESOURCE_DESC* rd, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv,
    ID3D12ProtectedResourceSession* ps, REFIID riid, void** pp)
{
    CallScope s;
    HRESULT hr = o_CCR1(d, hp, hf, rd, st, cv, ps, riid, pp);
    Finish(s, K_COMMITTED1, hr, ResourceBytes(d, rd), hp, hf, rd, 0);
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_CCR2(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
    const D3D12_RESOURCE_DESC1* rd, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv,
    ID3D12ProtectedResourceSession* ps, REFIID riid, void** pp)
{
    CallScope s;
    HRESULT hr = o_CCR2(d, hp, hf, rd, st, cv, ps, riid, pp);
    Finish(s, K_COMMITTED2, hr, ResourceBytes(d, DESC(rd)), hp, hf, DESC(rd), 0);
    return hr;
}
static HRESULT STDMETHODCALLTYPE h_CCR3(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
    const D3D12_RESOURCE_DESC1* rd, D3D12_BARRIER_LAYOUT lay, const D3D12_CLEAR_VALUE* cv,
    ID3D12ProtectedResourceSession* ps, UINT32 nf, const DXGI_FORMAT* fmts, REFIID riid, void** pp)
{
    CallScope s;
    HRESULT hr = o_CCR3(d, hp, hf, rd, lay, cv, ps, nf, fmts, riid, pp);
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
        Log("HITCH %.1f ms | creates %ld (committed %ld, heap %ld, placed %ld, reserved %ld, residency %ld) %.1f MB"
            " | thread-ms in creates %.1f (residency wait %.1f) | other KMT waits %.1f | top: %s",
            ms, creates, cnt[C_COMMITTED], cnt[C_HEAP], cnt[C_PLACED], cnt[C_RESERVED], cnt[C_RESIDENCY],
            by / 1048576.0, Ms(ct), Ms(wt), Ms(ot), tops[0] ? tops : "-");
    }

    if (Ms(now - g_stats.windowStart) >= 30000.0)
    {
        Log("STATS %.0f s: %ld frames, avg %.2f ms, worst %.1f ms, hitches %ld | creates %ld (%.1f/frame) %.1f MB"
            " | thread-ms in creates %.1f (residency wait %.1f)",
            Ms(now - g_stats.windowStart) / 1000.0, g_stats.frames, g_stats.frameMs / (g_stats.frames ? g_stats.frames : 1),
            g_stats.worstMs, g_stats.hitches, g_stats.creates,
            g_stats.creates / (double)(g_stats.frames ? g_stats.frames : 1), g_stats.mb, g_stats.createMs, g_stats.waitMs);
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
    if (sc) sc->Release();
    if (fac) fac->Release();
    if (q) q->Release();
    if (hwnd) DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, g_files.module);
    dev->Release();
    return 0;
}

// ----------------------------------------------------------------------------
// The device: found when the game creates it, or created by us if the game
// beat the hook (D3D12 returns the one device per adapter either way).
// ----------------------------------------------------------------------------
typedef HRESULT (WINAPI* PFN_D3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
static PFN_D3D12CreateDevice o_D3D12CreateDevice;
static volatile LONG g_devicePatched = 0;

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
    case C_PLACED:
    case C_RESERVED:
        if (e.kind == K_FRAME) break;
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

static DWORD WINAPI CaptureThread(LPVOID)
{
    WORD vk = KeyVk(g_cfg.captureKey);
    if (!vk) return 0;
    bool was = false;
    for (;;)
    {
        Sleep(30);
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
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
    Log("--- Ishimura Patch v%s (measurement build) --- %s", MOD_VERSION, ts);
    Log(g_files.iniCreated ? "ishimurapatch.ini not found - created with defaults"
                           : "config loaded from ishimurapatch.ini");
    Log("config: Logging = %d | HitchMs = %d | CaptureKey = %s", g_cfg.logging, g_cfg.hitchMs,
        g_cfg.captureKey[0] ? g_cfg.captureKey : "(off)");
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
    {
        if ((t = CreateThread(NULL, 0, WriterThread, NULL, 0, NULL))) CloseHandle(t);
        if ((t = CreateThread(NULL, 0, CaptureThread, NULL, 0, NULL))) CloseHandle(t);
    }
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
