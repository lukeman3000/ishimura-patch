// ============================================================================
//  ishimurapatch_probe.dll - development probe for Ishimura Patch
// ----------------------------------------------------------------------------
//  Reverse-engineering code that changes often: the mod watches this file and,
//  when it changes, loads a fresh copy into the running game, calls ProbeRun
//  once and unloads it, so a new probe needs no game restart. It must leave
//  nothing behind (no threads, no hooks). Logging goes through the mod's log.
//
//  Current probe (generation 13): NvidiaReflexSettings' values and types.
//  The settings inspector. Frostbite describes each class
//  in a static type record (decoded from Dead Space.exe, 10/18):
//    +0x00 name | +0x0E u16 size | +0x28 runtime type | +0x32 u16 field count
//    +0x38 runtime super | +0x40 field array
//  with 24-byte field records: +0x00 name, +0x0E u16 offset, +0x10 runtime
//  type. It finds the records, logs class layouts, and looks for the live
//  settings objects (fingerprints, type pointers, the short-name registry).
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

struct ProbeApi
{
    void (*log)(const char* line);
    LONGLONG qpcFreq;
};
static ProbeApi g_api;

static void Log(const char* fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    g_api.log(buf);
}
static inline LONGLONG Now() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
static inline double Ms(LONGLONG ticks) { return ticks * 1000.0 / (double)g_api.qpcFreq; }

#include "frostbite.h"

static void LogQwords(const char* label, const void* p, int count)
{
    for (int i = 0; i < count; ++i)
    {
        UINT64 q = 0;
        if (!SafeRead((BYTE*)p + i * 8, &q, 8)) { Log("  %s +0x%02X: (unreadable)", label, i * 8); return; }
        const ImgSec* s = InImage(q) ? SecOf((void*)q) : NULL;
        char str[48];
        SafeStr(q, str, sizeof(str));
        Log("  %s +0x%02X: %016llX%s%s%s%s", label, i * 8, (unsigned long long)q, s ? "  (" : "", s ? s->name : "",
            s ? ")" : "", str[0] && strlen(str) > 2 ? str : "");
    }
}

// A class to find: its fingerprint is a few u32 values at known offsets.
struct Fingerprint { const char* cls; struct { UINT16 off; UINT32 val; } v[6]; int n; };
// Only values Game.cfg sets for every platform (PoolSize is per platform).
static const Fingerprint FINGERPRINTS[] = {
    { "TextureStreamingSettings", { {0x74, 32}, {0x98, 10}, {0x48, 128000}, {0x78, 15}, {0x5C, 50000} }, 5 },
    { "MeshStreamingSettings",    { {0x70, 1000000}, {0x34, 32000} }, 2 },
};

// Walks committed readable memory in 64 KB chunks, each read under its own
// guard so one bad page skips 64 KB, not the region (0.3.0-dev's first scan
// gave up per region and covered almost nothing). Write-combined and
// uncached pages (GPU-visible heaps) are skipped: reading them is very slow.
static bool FpChunk(const Fingerprint& fp, BYTE* p, SIZE_T n, BYTE** found, int max, int& hits)
{
    __try
    {
        UINT32 first = fp.v[0].val; UINT16 off = fp.v[0].off;
        for (SIZE_T i = 0; i + 4 <= n; i += 4)
        {
            if (*(UINT32*)(p + i) != first) continue;
            BYTE* obj = p + i - off;
            if (((UINT64)obj & 7) != 0) continue;
            bool all = true;
            for (int k = 1; k < fp.n && all; ++k)
            {
                UINT32 v = 0;
                all = SafeRead(obj + fp.v[k].off, &v, 4) && v == fp.v[k].val;
            }
            if (all && hits < max) found[hits++] = obj;
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static int FindByFingerprint(const Fingerprint& fp, BYTE** found, int max, ScanStats& st)
{
    int hits = 0;
    ScanMemory([&](BYTE* p, SIZE_T n) { return FpChunk(fp, p, n, found, max, hits); }, st);
    return hits;
}

// Every 8-aligned place memory holds this exact pointer value.
static bool PtrChunk(UINT64 v, BYTE* p, SIZE_T n, BYTE** found, int max, int& hits)
{
    __try
    {
        for (SIZE_T i = 0; i + 8 <= n; i += 8)
            if (*(UINT64*)(p + i) == v && hits < max) found[hits++] = p + i;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static int FindPointer(UINT64 v, BYTE** found, int max, ScanStats& st)
{
    int hits = 0;
    ScanMemory([&](BYTE* p, SIZE_T n) { return PtrChunk(v, p, n, found, max, hits); }, st);
    return hits;
}

static void LogFieldValues(const BYTE* obj, const FieldDesc* f, int n, UINT16 size)
{
    for (int i = 0; i < n; ++i)
    {
        int width = (i + 1 < n ? f[i + 1].off : size) - f[i].off;
        BYTE v[8] = {};
        SafeRead(obj + f[i].off, v, width >= 8 ? 8 : (width > 0 ? width : 1));
        if (width == 1)
            Log("    +0x%03X %-48s %u", f[i].off, f[i].name, v[0]);
        else if (width >= 8)
        {
            char s[64]; SafeStr(*(UINT64*)v, s, sizeof(s));
            Log("    +0x%03X %-48s 0x%016llX %s", f[i].off, f[i].name, (unsigned long long)*(UINT64*)v, s);
        }
        else
            Log("    +0x%03X %-48s %d (as float %g)", f[i].off, f[i].name, *(INT32*)v, *(float*)v);
    }
}

// The image address of an exact NUL-terminated string (outside the VM sections).
static BYTE* FindImageString(const char* s)
{
    size_t len = strlen(s) + 1;
    for (int i = 0; i < g_nsecs; ++i)
    {
        if (!SecReadable(g_secs[i])) continue;
        BYTE* p = g_secs[i].p; SIZE_T n = g_secs[i].n;
        __try
        {
            for (BYTE* q = p + 1; q + len <= p + n; ++q)
                if (*q == (BYTE)s[0] && q[-1] == 0 && !memcmp(q, s, len)) return q;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return NULL;
}

// The config parser finds settings by their short name ("TextureStreaming"),
// so something maps that name to the live object: log every heap place that
// points at the name, its neighbours, and what those neighbours point at.
static void ProbeRegistry(const char* shortName, const Fingerprint* fp)
{
    BYTE* str = FindImageString(shortName);
    if (!str) { Log("SETTINGS: name \"%s\" NOT FOUND in the image", shortName); return; }
    BYTE* where[32];
    ScanStats st;
    int k = FindPointer((UINT64)str, where, 32, st);
    Log("SETTINGS: name \"%s\" @ %p, pointed at from %d place%s", shortName, str, k, k == 1 ? "" : "s");
    for (int j = 0; j < k && j < 16; ++j)
    {
        const ImgSec* s = SecOf(where[j]);
        Log("  name ref %d @ %p (%s)", j, where[j], s ? s->name : "heap");
        if (s) continue;
        LogQwords("  around", where[j] - 0x18, 7);
        for (int q = -3; q <= 3; ++q)
        {
            UINT64 v = 0;
            if (!SafeRead(where[j] + q * 8, &v, 8) || InImage(v) || v < 0x10000 || v > 0x7FFFFFFFFFFFull) continue;
            UINT32 probe[5] = {};
            bool any = false;
            if (fp)
                for (int f = 0; f < fp->n && f < 5; ++f) any |= SafeRead((BYTE*)v + fp->v[f].off, &probe[f], 4);
            Log("    neighbour %+d -> %016llX%s", q * 8, (unsigned long long)v, any ? " | at the fingerprint offsets:" : "");
            if (any)
                for (int f = 0; f < fp->n && f < 5; ++f)
                    Log("      +0x%02X = %u (expected %u)", fp->v[f].off, probe[f], fp->v[f].val);
            LogQwords("    object", (void*)v, 4);
        }
    }
}

static void LogFieldsTyped(const BYTE* obj, const FieldDesc* f, int n, UINT16 size)
{
    for (int i = 0; i < n; ++i)
    {
        char t[48]; TypeName(f[i].type, t, sizeof(t));
        int width = (i + 1 < n ? f[i + 1].off : size) - f[i].off;
        BYTE v[8] = {};
        SafeRead(obj + f[i].off, v, width >= 8 ? 8 : (width > 0 ? width : 1));
        if (!strcmp(t, "Boolean") || width == 1)
            Log("    +0x%03X %-48s %-10s %u", f[i].off, f[i].name, t, v[0]);
        else if (!strcmp(t, "Float32"))
            Log("    +0x%03X %-48s %-10s %g", f[i].off, f[i].name, t, *(float*)v);
        else if (!strcmp(t, "CString"))
        {
            char s[64]; UINT64 p = *(UINT64*)v; s[0] = 0;
            if (p && SafeRead((void*)p, s, sizeof(s) - 1)) s[sizeof(s) - 1] = 0;
            for (size_t k = 0; s[k]; ++k) if ((unsigned char)s[k] < 0x20 || (unsigned char)s[k] > 0x7e) { s[k] = 0; break; }
            Log("    +0x%03X %-48s %-10s \"%s\"", f[i].off, f[i].name, t, s);
        }
        else if (width >= 8)
            Log("    +0x%03X %-48s %-10s 0x%016llX", f[i].off, f[i].name, t, (unsigned long long)*(UINT64*)v);
        else
            Log("    +0x%03X %-48s %-10s %d", f[i].off, f[i].name, t, *(INT32*)v);
    }
}

static void Inspect(const char* why)
{
    LONGLONG t0 = Now();
    ImgInit();
    Log("SETTINGS: inspection (%s) | image 0x%p, %llu MB", why, g_imgBase, (unsigned long long)(g_imgSize >> 20));
    static FieldDesc fields[128];
    for (const Shape& sh : SHAPES)
    {
        BYTE* rec = FindTypeRecord(sh.cls);
        if (!rec) { Log("SETTINGS: %s record NOT FOUND", sh.cls); continue; }
        UINT16 size = 0; UINT64 rt = 0;
        int n = ReadFields(rec, fields, 128, &size, &rt);
        BYTE* objs[16];
        int hits = 0;
        ScanStats st;
        LONGLONG s0 = Now();
        ScanMemory([&](BYTE* p, SIZE_T len) { return ShapeChunk(sh, p, len, objs, 16, hits); }, st);
        Log("SETTINGS: %s structural scan %.0f ms (%llu MB): %d match%s", sh.cls, Ms(Now() - s0),
            (unsigned long long)(st.bytes >> 20), hits, hits == 1 ? "" : "es");
        for (int j = 0; j < hits; ++j)
        {
            const ImgSec* s = SecOf(objs[j]);
            UINT64 vt = *(UINT64*)objs[j];
            const ImgSec* vs = SecOf((void*)vt);
            Log("SETTINGS: %s candidate %d @ %p (%s) | vtable %016llX (%s)", sh.cls, j, objs[j], s ? s->name : "heap",
                (unsigned long long)vt, vs ? vs->name : "?");
            // the real objects' vtables sit in .link, next to the short name
            if (vs && !strcmp(vs->name, ".link"))
            {
                LogQwords("header", objs[j], 4);
                LogFieldsTyped(objs[j], fields, n, size);
            }
        }
    }
    Log("SETTINGS: inspection done in %.0f ms", Ms(Now() - t0));
}

// Generation 4: write settings. Reads ishimurapatch_settings.txt beside the
// mod, lines "Class.Field = value" ('#' starts a comment), and writes each
// value into every live object of that class: the objects the structural
// scan matches whose vtable sits in .link (the configured object and the
// engine's defaults object both; writing both keeps a change from being
// undone if the engine re-copies defaults). Types come from the field's own
// type record; only Boolean, Uint32, Int32 and Float32 are written.
static void ApplySettings()
{
    char path[MAX_PATH];
    HMODULE mod = GetModuleHandleA("ishimurapatch.asi");
    if (!mod || !GetModuleFileNameA(mod, path, MAX_PATH)) { Log("SETTINGS: can't locate the mod's folder"); return; }
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    strcpy_s(slash + 1, path + sizeof(path) - slash - 1, "ishimurapatch_settings.txt");
    FILE* f = NULL;
    if (fopen_s(&f, path, "r") != 0 || !f) { Log("SETTINGS: no %s - nothing to write", path); return; }
    char line[256];
    int written = 0;
    while (fgets(line, sizeof(line), f))
    {
        char* hash = strchr(line, '#'); if (hash) *hash = 0;
        char cls[96], fld[96], val[64];
        if (sscanf_s(line, " %95[^. ] . %95[^= ] = %63s", cls, (unsigned)sizeof(cls), fld, (unsigned)sizeof(fld), val, (unsigned)sizeof(val)) != 3) continue;
        written += WriteSetting(cls, fld, val, "SETTINGS: WRITE");
    }
    fclose(f);
    Log("SETTINGS: %d value%s written", written, written == 1 ? "" : "s");
}

// Generation 10 (9 + the loader and Reflex settings' values): a live test of FlurryLoaderSettings through the new
// type-based lookup (frostbite.h FindObjectsByType), from
// ishimurapatch_settings.txt like generation 4.
static void DumpNamed(BYTE* obj, const char* cls)
{
    static FieldDesc fields[128];
    BYTE* rec = FindTypeRecord(cls);
    if (!rec) return;
    UINT16 size = 0; UINT64 rt = 0;
    int n = ReadFields(rec, fields, 128, &size, &rt);
    LogFieldsTyped(obj, fields, n, size);
}

static void ApplySettingsFile()
{
    char path[MAX_PATH];
    HMODULE mod = GetModuleHandleA("ishimurapatch.asi");
    if (!mod || !GetModuleFileNameA(mod, path, MAX_PATH)) return;
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    strcpy_s(slash + 1, path + sizeof(path) - slash - 1, "ishimurapatch_settings.txt");
    FILE* f = NULL;
    if (fopen_s(&f, path, "r") != 0 || !f) { Log("SETTINGS: no %s", path); return; }
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char* hash = strchr(line, '#'); if (hash) *hash = 0;
        char cls[96], fld[96], val[64];
        if (sscanf_s(line, " %95[^. ] . %95[^= ] = %63s", cls, (unsigned)sizeof(cls), fld, (unsigned)sizeof(fld), val, (unsigned)sizeof(val)) == 3)
            WriteSetting(cls, fld, val, "SETTINGS: WRITE");
    }
    fclose(f);
}

// Generation 14: Dx12DisplaySettings' Reflex fields (13: NvidiaReflexSettings' live values and field types (for an
// [Engine] Reflex switch).
extern "C" __declspec(dllexport) void ProbeRun(const ProbeApi* api)
{
    g_api = *api;
    ImgInit();
    BYTE* objs[8];
    static FieldDesc fields[256];
    BYTE* rec = FindTypeRecord("Dx12DisplaySettings");
    UINT16 size = 0; UINT64 rt = 0;
    int nf = rec ? ReadFields(rec, fields, 256, &size, &rt) : 0;
    int n = FindObjectsByType("Dx12DisplaySettings", objs, 8);
    Log("SETTINGS: Dx12DisplaySettings: %d object%s, %d fields", n, n == 1 ? "" : "s", nf);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < nf; ++i)
            if (strstr(fields[i].name, "Reflex") || strstr(fields[i].name, "Latency") || strstr(fields[i].name, "Sleep"))
            {
                BYTE v = 0; SafeRead(objs[j] + fields[i].off, &v, 1);
                char t[48]; TypeName(fields[i].type, t, sizeof(t));
                Log("SETTINGS:   %p +0x%03X %s (%s) = %u", objs[j], fields[i].off, fields[i].name, t, v);
            }
}
