#include "config.hpp"

#include <cstring>
#include <CTRPluginFramework.hpp>

using namespace CTRPluginFramework;

namespace mc3d::cfg
{
    namespace
    {
        constexpr const char *kPath = "/luma/plugins/00040000001B8700/mc3d_cfg.txt";
        u32  s_hooks = kAll;
        bool s_fromFile = false;
        bool s_irrst = false;

        // v0.2 stereo start-up options (value -1 = not set)
        struct Opt { const char *key; int value; };
        Opt s_opts[] = { { "mode", -1 }, { "tdepth", -1 }, { "flatdepth", -1 }, { "comp", -1 },
                         { "leftvram", -1 }, { "menupop", -1 }, { "log", -1 }, { "osd", -1 },
                         // v0.3
                         { "lock", -1 }, { "rd", -1 }, { "rdauto", -1 }, { "rd3d", -1 },
                         { "leaves", -1 }, { "farfog", -1 },
                         { "guardheap", -1 }, { "gov", -1 },
                         // v0.3.1
                         { "hold", -1 },
                         // v0.3.2 (read by stereo_flat.cpp but missing here until v0.3.3)
                         { "hud", -1 },
                         // v0.3.3
                         { "abswap", -1 } };

        int ParseValue(const char *v, const char *ve)
        {
            const size_t n = static_cast<size_t>(ve - v);
            if ((n == 2 && !std::strncmp(v, "on", 2)) || (n == 5 && !std::strncmp(v, "depth", 5)))
                return 1;
            if ((n == 3 && !std::strncmp(v, "off", 3)) || (n == 4 && !std::strncmp(v, "flat", 4)))
                return 0;
            // v0.3: mode=comfort (2) / alternate (1, same as "depth"); leaves=auto (2)
            if ((n == 7 && !std::strncmp(v, "comfort", 7)) || (n == 4 && !std::strncmp(v, "auto", 4)))
                return 2;
            if (n == 9 && !std::strncmp(v, "alternate", 9))
                return 1;
            int x = 0;
            bool any = false;
            for (; v < ve && *v >= '0' && *v <= '9'; v++, any = true)
                x = x * 10 + (*v - '0');
            return any && x < 1000 ? x : -1;
        }

        struct Name { const char *name; u32 bits; };
        const Name kNames[] = {
            { "present", kPresent }, { "init", kInit }, { "b2l", kB2L }, { "swap", kSwap },
            { "world", kWorld }, { "draw", kDraw }, { "sync", kSync }, { "rebuild", kRebuild },
            { "jobs", kJobs }, { "split", kSplit }, { "proj", kProj }, { "prof", kProf },
            { "comfort", kComfort }, { "world", kWorldOpts }, { "input", kInput }, { "all", kAll }, { "none", 0 },
        };

        bool IsSep(char c) { return c == ',' || c == ' ' || c == '\t'; }

        u32 ParseList(const char *s, const char *end)
        {
            u32 bits = 0;
            while (s < end)
            {
                while (s < end && IsSep(*s)) s++;
                const char *t = s;
                while (t < end && !IsSep(*t)) t++;
                const size_t n = static_cast<size_t>(t - s);
                for (const Name &nm : kNames)
                    if (n && std::strlen(nm.name) == n && std::strncmp(nm.name, s, n) == 0)
                        bits |= nm.bits;
                s = t;
            }
            return bits;
        }
    }

    void Load()
    {
        File f;
        if (File::Open(f, kPath, File::READ) != File::SUCCESS)
            return;
        char buf[512];
        const u64 size = f.GetSize();
        const u32 len = size < sizeof(buf) - 1 ? static_cast<u32>(size) : sizeof(buf) - 1;
        if (f.Read(buf, len) != File::SUCCESS)
        {
            f.Close();
            return;
        }
        f.Close();
        buf[len] = 0;

        const char *p = buf, *end = buf + len;
        // Some Windows editors start the file with a UTF-8 BOM (EF BB BF);
        // without this skip the first line would never match.
        if (len >= 3 && static_cast<u8>(buf[0]) == 0xEF && static_cast<u8>(buf[1]) == 0xBB &&
            static_cast<u8>(buf[2]) == 0xBF)
            p += 3;
        while (p < end)
        {
            // Lines end in LF, CRLF or CR (the empty "line" between CR and LF is harmless).
            const char *eol = p;
            while (eol < end && *eol != '\n' && *eol != '\r') eol++;
            const char *hash = static_cast<const char *>(std::memchr(p, '#', eol - p));
            const char *lineEnd = hash ? hash : eol;
            while (p < lineEnd && IsSep(*p)) p++;
            // key [spaces] = [spaces] value [spaces]
            const char *key = p;
            while (p < lineEnd && *p != '=' && *p != ' ' && *p != '\t') p++;
            const size_t keyLen = static_cast<size_t>(p - key);
            while (p < lineEnd && (*p == ' ' || *p == '\t')) p++;
            if (p < lineEnd && *p == '=')
            {
                const char *v = p + 1;
                while (v < lineEnd && (*v == ' ' || *v == '\t')) v++;
                const char *ve = lineEnd;
                while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
                if (keyLen == 5 && std::strncmp(key, "hooks", 5) == 0)
                {
                    s_hooks = ParseList(v, ve);
                    s_fromFile = true;
                }
                else
                {
                    for (Opt &o : s_opts)
                        if (std::strlen(o.key) == keyLen && std::strncmp(key, o.key, keyLen) == 0)
                            o.value = ParseValue(v, ve);
                }
                if (keyLen == 5 && std::strncmp(key, "irrst", 5) == 0)
                {
                    s_irrst = (ve - v == 2 && std::strncmp(v, "on", 2) == 0) ||
                              (ve - v == 1 && *v == '1');
                }
            }
            p = eol + 1;
        }
    }

    bool        Hook(u32 bits) { return (s_hooks & bits) == bits; }
    u32         Hooks()        { return s_hooks; }
    bool        FromFile()     { return s_fromFile; }
    const char *Path()         { return kPath; }
    bool        IrrstAllowed() { return s_irrst; }
    int StereoOpt(const char *key)
    {
        for (const Opt &o : s_opts)
            if (!std::strcmp(o.key, key))
                return o.value;
        return -1;
    }
}
