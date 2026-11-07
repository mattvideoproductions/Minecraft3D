#pragma once
// SD paths of the plugin's own files. Luma3DS loads the plugin from
// /luma/plugins/<title ID>/, so the files live next to it in the folder of
// the running title (USA 00040000001B8700, EUR 000400000017CA00).
#include <cstddef>

namespace mc3d::paths
{
    const char *Dir();          // "/luma/plugins/<16 hex digits>/"
    const char *Config();       // <Dir>mc3d_cfg.txt
    const char *Log();          // <Dir>mc3d_log.txt
    const char *LogOld();       // <Dir>mc3d_log_old.txt
    void        Csv(char *out, std::size_t size, int number);   // <Dir>mc3d_profile_%03d.csv
}
