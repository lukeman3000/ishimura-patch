// ============================================================================
//  frostbite.h - Frostbite type info and live settings objects (shared by
//  ishimurapatch.cpp and probe.cpp)
// ----------------------------------------------------------------------------
//  Static type records in Dead Space.exe (decoded 10/18):
//    +0x00 name | +0x0E u16 size | +0x28 runtime type | +0x32 u16 field count
//    +0x38 runtime super | +0x40 field array
//  24-byte field records: +0x00 name, +0x0E u16 offset, +0x10 runtime type;
//  a runtime type's +0x00 points at its static record (so its name).
//  Live settings objects carry no type pointer and aren't reachable by name
//  (lookups are by hash); they are found by shape: a vtable in .link first,
//  the Boolean block all 0/1, known switches on, sane counts. Each class has
//  two such objects, the configured one and the engine's defaults.
//
//  The including file defines Log(fmt, ...) before including this.
// ============================================================================
#pragma once
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ImgSec { char name[9]; BYTE* p; SIZE_T n; };
static ImgSec g_secs[96];
static int    g_nsecs = 0;
static BYTE*  g_imgBase = NULL;
static SIZE_T g_imgSize = 0;

static void ImgInit()
{
    if (g_imgBase) return;
    g_imgBase = (BYTE*)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_imgBase + ((IMAGE_DOS_HEADER*)g_imgBase)->e_lfanew);
    g_imgSize = nt->OptionalHeader.SizeOfImage;
    IMAGE_SECTION_HEADER* sh = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections && g_nsecs < 96; ++i)
    {
        ImgSec& s = g_secs[g_nsecs++];
        memcpy(s.name, sh[i].Name, 8); s.name[8] = 0;
        s.p = g_imgBase + sh[i].VirtualAddress;
        s.n = sh[i].Misc.VirtualSize;
    }
}
static bool InImage(UINT64 v) { return v >= (UINT64)g_imgBase && v < (UINT64)g_imgBase + g_imgSize; }
static const ImgSec* SecOf(const void* p)
{
    for (int i = 0; i < g_nsecs; ++i)
        if ((BYTE*)p >= g_secs[i].p && (BYTE*)p < g_secs[i].p + g_secs[i].n) return &g_secs[i];
    return NULL;
}
// Copies [p, p+n) or fails, never faults (pages here can be absent or guarded).
static bool SafeRead(const void* p, void* out, SIZE_T n)
{
    __try { memcpy(out, p, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static const char* SafeStr(UINT64 v, char* buf, size_t n)
{
    buf[0] = 0;
    if (!InImage(v)) return buf;
    if (!SafeRead((void*)v, buf, n - 1)) { buf[0] = 0; return buf; }
    buf[n - 1] = 0;
    for (size_t i = 0; buf[i]; ++i) if ((unsigned char)buf[i] < 0x20 || (unsigned char)buf[i] > 0x7e) { buf[i] = 0; break; }
    return buf;
}
static bool SecReadable(const ImgSec& s)
{
    // the Denuvo VM sections are large and of no use here
    return strcmp(s.name, ".00cfg") && strcmp(s.name, ".arch") && strcmp(s.name, ".xcode");
}

// The static record of a class, by name: the string, then the .srdata record
// pointing at it whose field array is an image pointer.
static BYTE* FindTypeRecord(const char* cls)
{
    size_t len = strlen(cls) + 1;
    for (int i = 0; i < g_nsecs; ++i)
    {
        if (!SecReadable(g_secs[i])) continue;
        BYTE* p = g_secs[i].p; SIZE_T n = g_secs[i].n;
        __try
        {
            for (BYTE* q = p + 1; q + len <= p + n; ++q)
            {
                if (*q != (BYTE)cls[0] || q[-1] != 0 || memcmp(q, cls, len)) continue;
                UINT64 sva = (UINT64)q;
                for (int k = 0; k < g_nsecs; ++k)
                {
                    if (strcmp(g_secs[k].name, ".srdata")) continue;
                    UINT64* r = (UINT64*)g_secs[k].p;
                    SIZE_T cnt = g_secs[k].n / 8;
                    for (SIZE_T j = 0; j + 9 < cnt; ++j)
                        if (r[j] == sva && InImage(r[j + 8])) return (BYTE*)&r[j];
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return NULL;
}

struct FieldDesc { char name[64]; UINT16 off; UINT64 type; };
static int ReadFields(BYTE* rec, FieldDesc* out, int max, UINT16* size, UINT64* rt)
{
    UINT16 sz = 0, nf = 0; UINT64 fa = 0;
    if (!SafeRead(rec + 0x0E, &sz, 2) || !SafeRead(rec + 0x32, &nf, 2) || !SafeRead(rec + 0x40, &fa, 8) ||
        !SafeRead(rec + 0x28, rt, 8)) return 0;
    *size = sz;
    int n = nf < max ? nf : max;
    for (int i = 0; i < n; ++i)
    {
        BYTE f[24];
        if (!SafeRead((BYTE*)fa + i * 24, f, 24)) return i;
        SafeStr(*(UINT64*)f, out[i].name, sizeof(out[i].name));
        out[i].off = *(UINT16*)(f + 0x0E);
        out[i].type = *(UINT64*)(f + 0x10);
    }
    // by offset, so a field's size is the gap to the next one
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && out[j].off < out[j - 1].off; --j) { FieldDesc t = out[j]; out[j] = out[j - 1]; out[j - 1] = t; }
    return n;
}

struct ScanStats { UINT64 bytes, regions, faults; };
template <class F>
static void ScanMemory(F visit, ScanStats& st)
{
    st = ScanStats();
    MEMORY_BASIC_INFORMATION mbi;
    BYTE* a = NULL;
    while (VirtualQuery(a, &mbi, sizeof(mbi)) == sizeof(mbi))
    {
        BYTE* next = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
        DWORD pr = mbi.Protect;
        bool ok = mbi.State == MEM_COMMIT && !(pr & (PAGE_GUARD | PAGE_NOACCESS | PAGE_WRITECOMBINE | PAGE_NOCACHE)) &&
                  (pr & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
        if (ok && !((BYTE*)mbi.BaseAddress >= g_imgBase && (BYTE*)mbi.BaseAddress < g_imgBase + g_imgSize &&
                    (pr & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))))   // not the game's own code
        {
            st.regions++;
            for (SIZE_T c = 0; c < mbi.RegionSize; c += 0x10000)
            {
                BYTE* p = (BYTE*)mbi.BaseAddress + c;
                SIZE_T n = mbi.RegionSize - c < 0x10000 ? mbi.RegionSize - c : 0x10000;
                if (!visit(p, n)) st.faults++;
                else st.bytes += n;
            }
        }
        if (next <= a) break;
        a = next;
    }
}

// The name of a field's type: its runtime type's +0x00 points at the type's
// static record, whose +0x00 is the name (seen on 10/18: field 0 -> CString).
static const char* TypeName(UINT64 rt, char* buf, size_t n)
{
    UINT64 rec = 0, name = 0;
    buf[0] = 0;
    if (!SafeRead((void*)rt, &rec, 8) || !InImage(rec) || !SafeRead((void*)rec, &name, 8)) return buf;
    return SafeStr(name, buf, n);
}

// A structural fingerprint: an image pointer first (the vtable), a block of
// Boolean fields that are all 0 or 1, some switches that must be on, and
// count fields that must be small and positive. No guessed config values.
struct Shape
{
    const char* cls;
    UINT16 boolLo, boolHi;          // [lo, hi) all bytes 0/1
    UINT16 ones[6]; int nOnes;      // these bytes == 1
    UINT16 counts[4]; int nCounts;  // these u32 in 1..1000000
};
static const Shape SHAPES[] = {
    { "TextureStreamingSettings", 0xB8, 0xD8, { 0xBA /*Enable*/, 0xCF /*MipmapsEnable*/, 0xC9 /*UpdateEnable*/, 0xCD /*LoadMipmapsEnable*/ }, 4,
      { 0x74 /*MaxPendingLoadCount*/, 0x98 /*MaxFrameTextureCreateCount*/ }, 2 },
    { "MeshStreamingSettings", 0x98, 0xB9, { 0x98 /*Enable*/, 0xA3 /*UpdateEnable*/ }, 2,
      { 0x54 /*MaxPendingLoadCount*/, 0x70 /*PoolSize*/ }, 2 },
};

static bool ShapeChunk(const Shape& sh, BYTE* p, SIZE_T n, BYTE** found, int max, int& hits)
{
    __try
    {
        for (SIZE_T i = 0; i + 8 <= n; i += 8)
        {
            UINT64 vt = *(UINT64*)(p + i);
            if (!InImage(vt)) continue;
            BYTE* obj = p + i;
            BYTE b[64];
            if (!SafeRead(obj + sh.boolLo, b, sh.boolHi - sh.boolLo)) continue;
            bool ok = true;
            for (int k = 0; k < sh.boolHi - sh.boolLo && ok; ++k) ok = b[k] <= 1;
            for (int k = 0; k < sh.nOnes && ok; ++k) ok = b[sh.ones[k] - sh.boolLo] == 1;
            for (int k = 0; k < sh.nCounts && ok; ++k)
            {
                UINT32 c = 0;
                ok = SafeRead(obj + sh.counts[k], &c, 4) && c >= 1 && c <= 1000000;
            }
            if (ok && hits < max) found[hits++] = obj;
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static int FindObjects(const Shape& sh, BYTE** objs, int max)
{
    BYTE* all[32];
    int hits = 0;
    ScanStats st;
    ScanMemory([&](BYTE* p, SIZE_T len) { return ShapeChunk(sh, p, len, all, 32, hits); }, st);
    int n = 0;
    for (int j = 0; j < hits && n < max; ++j)
    {
        const ImgSec* vs = SecOf((void*)*(UINT64*)all[j]);
        if (vs && !strcmp(vs->name, ".link")) objs[n++] = all[j];
    }
    return n;
}

// Any settings object by its class name. A live settings object starts with
// its vtable (in .link) and its runtime type (in .debug); every settings
// vtable's slot 0 is the same accessor, "mov rax,[rcx+8]; ret", returning that
// type (probe generations 6-8, 10/19). The name-to-type registry entries also
// pair a .link and a .debug pointer, but their "vtable" points at the class
// name, so the slot 0 check tells them apart.
static bool IsTypeAccessor(UINT64 fn)
{
    static const BYTE code[5] = { 0x48, 0x8B, 0x41, 0x08, 0xC3 };
    BYTE b[5];
    return InImage(fn) && SafeRead((void*)fn, b, 5) && !memcmp(b, code, 5);
}

static bool TypedChunk(const char* cls, BYTE* p, SIZE_T n, BYTE** found, int max, int& hits,
                       const ImgSec* link, const ImgSec* dbg)
{
    __try
    {
        for (SIZE_T i = 0; i + 16 <= n; i += 8)
        {
            BYTE* vt = *(BYTE**)(p + i);
            BYTE* ty = *(BYTE**)(p + i + 8);
            if (vt < link->p || vt + 8 > link->p + link->n || ty < dbg->p || ty >= dbg->p + dbg->n) continue;
            if (!IsTypeAccessor(*(UINT64*)vt)) continue;
            char tn[96];
            if (strcmp(TypeName((UINT64)ty, tn, sizeof(tn)), cls)) continue;
            if (hits < max) found[hits++] = p + i;
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static int FindObjectsByType(const char* cls, BYTE** objs, int max)
{
    const ImgSec *link = NULL, *dbg = NULL;
    for (int i = 0; i < g_nsecs; ++i)
    {
        if (!strcmp(g_secs[i].name, ".link")) link = &g_secs[i];
        if (!strcmp(g_secs[i].name, ".debug")) dbg = &g_secs[i];
    }
    if (!link || !dbg) return 0;
    int hits = 0;
    ScanStats st;
    ScanMemory([&](BYTE* p, SIZE_T len) { return TypedChunk(cls, p, len, objs, max, hits, link, dbg); }, st);
    int n = 0;
    for (int j = 0; j < hits; ++j) if (!SecOf(objs[j])) objs[n++] = objs[j];   // heap objects only
    return n;
}

// One "Class.Field = value" setting, resolved once (the live objects, the
// field's offset and type, the value encoded for that type) and applied as
// often as needed: Apply only writes objects whose value differs. Types come
// from the field's own type record; Boolean, Uint32, Int32 and Float32 only.
struct SettingRef
{
    char  cls[96], fld[96], val[64], type[48];
    BYTE* objs[4];
    int   k, width;
    UINT16 off;
    BYTE  bytes[4];
    bool  enum4;        // a 4-byte enum, written as an Int32
};

// The game's own value of every field the mod has written, recorded at the
// first write by any thread (the startup writer and the [Engine] thread both
// write; a record per SettingRef took the other's value for the game's,
// 10/19), and written back when a setting is taken out.
struct OrigEntry { BYTE* addr; BYTE v[4]; };
static OrigEntry     g_orig[256];
static int           g_nOrig = 0;
static volatile LONG g_origLock = 0;

static void OrigLock()   { while (InterlockedExchange(&g_origLock, 1)) Sleep(0); }
static void OrigUnlock() { InterlockedExchange(&g_origLock, 0); }

static void OrigRecord(BYTE* addr, const BYTE* old)
{
    OrigLock();
    int i = 0;
    while (i < g_nOrig && g_orig[i].addr != addr) ++i;
    if (i == g_nOrig && g_nOrig < 256) { g_orig[i].addr = addr; memcpy(g_orig[i].v, old, 4); ++g_nOrig; }
    OrigUnlock();
}

// Copies the recorded value into out; with forget, drops the record.
static bool OrigTake(BYTE* addr, BYTE* out, bool forget)
{
    OrigLock();
    int i = 0;
    while (i < g_nOrig && g_orig[i].addr != addr) ++i;
    bool ok = i < g_nOrig;
    if (ok)
    {
        memcpy(out, g_orig[i].v, 4);
        if (forget) g_orig[i] = g_orig[--g_nOrig];
    }
    OrigUnlock();
    return ok;
}

// The byte size a runtime type's static record declares (+0x0E), 0 if unknown.
static UINT16 TypeSize(UINT64 rt)
{
    UINT64 rec = 0; UINT16 size = 0;
    if (!SafeRead((void*)rt, &rec, 8) || !InImage(rec) || !SafeRead((BYTE*)rec + 0x0E, &size, 2)) return 0;
    return size;
}

// r.val encoded as r.type into r.bytes; false for a type not written. An enum
// (NvidiaReflexMode: its own named type, 4 bytes) is written as an Int32.
static bool EncodeSetting(SettingRef& r)
{
    if (!strcmp(r.type, "Boolean"))      { r.bytes[0] = (BYTE)(atoi(r.val) != 0); r.width = 1; }
    else if (!strcmp(r.type, "Uint32"))  { UINT32 v = (UINT32)strtoul(r.val, NULL, 10); memcpy(r.bytes, &v, 4); r.width = 4; }
    else if (!strcmp(r.type, "Int32"))   { INT32 v = (INT32)strtol(r.val, NULL, 10); memcpy(r.bytes, &v, 4); r.width = 4; }
    else if (!strcmp(r.type, "Float32")) { float v = (float)atof(r.val); memcpy(r.bytes, &v, 4); r.width = 4; }
    else if (r.enum4) { INT32 v = (INT32)strtol(r.val, NULL, 10); memcpy(r.bytes, &v, 4); r.width = 4; }
    else return false;
    return true;
}

static bool ResolveSetting(SettingRef& r, const char* tag, bool quiet = false)
{
    r.k = 0;
    FieldDesc fields[128];   // not static: the startup writer and the [Engine] thread both resolve
    BYTE* rec = FindTypeRecord(r.cls);
    UINT16 size = 0; UINT64 rt = 0;
    int n = rec ? ReadFields(rec, fields, 128, &size, &rt) : 0;
    const FieldDesc* fd = NULL;
    for (int i = 0; i < n; ++i) if (!strcmp(fields[i].name, r.fld)) fd = &fields[i];
    if (!fd) { Log("%s: %s.%s - no such field%s", tag, r.cls, r.fld, rec ? "" : " (class record NOT FOUND)"); return false; }
    TypeName(fd->type, r.type, sizeof(r.type));
    r.off = fd->off;
    r.enum4 = false;
    if (r.type[0] && strcmp(r.type, "Boolean") && strcmp(r.type, "Uint32") && strcmp(r.type, "Int32") && strcmp(r.type, "Float32"))
    {
        // a named 4-byte type whose next field starts 4 bytes on: an enum
        UINT16 next = 0xFFFF;
        for (int i = 0; i < n; ++i) if (fields[i].off > r.off && fields[i].off < next) next = fields[i].off;
        r.enum4 = TypeSize(fd->type) == 4 && (next == 0xFFFF || next - r.off == 4);
    }
    if (!EncodeSetting(r))
    {
        // an unnamed type is type info not filled in yet (startup): quietly retried
        if (!quiet || r.type[0]) Log("%s: %s.%s is a %s - not written", tag, r.cls, r.fld, r.type[0] ? r.type : "type of unknown name");
        return false;
    }
    r.k = FindObjectsByType(r.cls, r.objs, 4);
    if (!r.k) { if (!quiet) Log("%s: %s.%s - no live object found", tag, r.cls, r.fld); return false; }
    return true;
}

static void LogValue(const char* tag, const SettingRef& r, int j, const BYTE* old, const BYTE* now)
{
    if (r.width == 1) Log("%s: %s.%s (%s) @ %p: %u -> %u", tag, r.cls, r.fld, r.type, r.objs[j], old[0], now[0]);
    else if (!strcmp(r.type, "Float32")) Log("%s: %s.%s (%s) @ %p: %g -> %g", tag, r.cls, r.fld, r.type, r.objs[j], *(const float*)old, *(const float*)now);
    else Log("%s: %s.%s (%s) @ %p: %d -> %d", tag, r.cls, r.fld, r.type, r.objs[j], *(const INT32*)old, *(const INT32*)now);
}

// Returns how many objects were written (already-correct ones are skipped);
// -1 if an object can no longer be read (it moved: resolve again).
static int ApplySetting(SettingRef& r, const char* tag)
{
    int done = 0;
    for (int j = 0; j < r.k; ++j)
    {
        BYTE old[4] = {};
        if (!SafeRead(r.objs[j] + r.off, old, r.width)) return -1;
        if (!memcmp(old, r.bytes, r.width)) continue;
        __try { memcpy(r.objs[j] + r.off, r.bytes, r.width); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("%s: %s.%s write FAILED @ %p", tag, r.cls, r.fld, r.objs[j]); return -1; }
        OrigRecord(r.objs[j] + r.off, old);
        LogValue(tag, r, j, old, r.bytes);
        ++done;
    }
    return done;
}

// Writes back each object's value from before the mod's first write (a line
// taken out of [Engine] live). Returns how many were written.
static int PutBackSetting(SettingRef& r, const char* tag)
{
    int done = 0;
    for (int j = 0; j < r.k; ++j)
    {
        BYTE old[4] = {}, orig[4] = {};
        if (!OrigTake(r.objs[j] + r.off, orig, true) || !SafeRead(r.objs[j] + r.off, old, r.width)) continue;
        if (!memcmp(old, orig, r.width)) continue;
        __try { memcpy(r.objs[j] + r.off, orig, r.width); }
        __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        LogValue(tag, r, j, old, orig);
        ++done;
    }
    return done;
}

// Whether the mod has written this setting (so taking it out puts a value back).
static bool SettingWritten(const SettingRef& r)
{
    BYTE v[4];
    for (int j = 0; j < r.k; ++j) if (OrigTake(r.objs[j] + r.off, v, false)) return true;
    return false;
}

// Both steps at once (the probe's one-shot writes).
static int WriteSetting(const char* cls, const char* fld, const char* val, const char* tag)
{
    SettingRef r = {};
    strncpy_s(r.cls, sizeof(r.cls), cls, _TRUNCATE);
    strncpy_s(r.fld, sizeof(r.fld), fld, _TRUNCATE);
    strncpy_s(r.val, sizeof(r.val), val, _TRUNCATE);
    if (!ResolveSetting(r, tag)) return 0;
    int n = ApplySetting(r, tag);
    return n > 0 ? n : 0;
}
