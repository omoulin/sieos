/*
 * multicall.cpp - One program for all of WebKit's processes: /bin/web is
 * also the WebProcess and the NetworkProcess (their names in
 * /usr/libexec/wpe-webkit-2.0 are links to it). Each would hold all of
 * WebKit anyway (static linking); one file, read once, its code shared by
 * every process started from it (libposix's program images, by file).
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <string.h>

namespace WebKit {
int WebProcessMain(int argc, char** argv);
int NetworkProcessMain(int argc, char** argv);
}

/* 1 if argv[0] names one of WebKit's processes, which then ran: its status in *status */
extern "C" int sieos_webkit_process(int argc, char** argv, int* status)
{
    const char* name = argc > 0 ? strrchr(argv[0], '/') : nullptr;
    name = name ? name + 1 : argc > 0 ? argv[0] : "";
    if (!strcmp(name, "WPEWebProcess"))
        *status = WebKit::WebProcessMain(argc, argv);
    else if (!strcmp(name, "WPENetworkProcess"))
        *status = WebKit::NetworkProcessMain(argc, argv);
    else
        return 0;
    return 1;
}
