#include "paths.hpp"

#include <cstdio>
#include <CTRPluginFramework.hpp>

using CTRPluginFramework::Process;

namespace mc3d::paths
{
    namespace
    {
        // Built once from the running title ID. Every thread that gets here
        // first writes the same bytes, so a race is harmless.
        char s_dir[40], s_cfg[64], s_log[64], s_logOld[64];
        bool s_ready = false;

        void Init()
        {
            if (s_ready)
                return;
            const unsigned long long tid = Process::GetTitleID();
            std::snprintf(s_dir, sizeof(s_dir), "/luma/plugins/%016llX/", tid);
            std::snprintf(s_cfg, sizeof(s_cfg), "%smc3d_cfg.txt", s_dir);
            std::snprintf(s_log, sizeof(s_log), "%smc3d_log.txt", s_dir);
            std::snprintf(s_logOld, sizeof(s_logOld), "%smc3d_log_old.txt", s_dir);
            s_ready = true;
        }
    }

    const char *Dir()    { Init(); return s_dir; }
    const char *Config() { Init(); return s_cfg; }
    const char *Log()    { Init(); return s_log; }
    const char *LogOld() { Init(); return s_logOld; }

    void Csv(char *out, std::size_t size, int number)
    {
        Init();
        std::snprintf(out, size, "%smc3d_profile_%03d.csv", s_dir, number);
    }
}
