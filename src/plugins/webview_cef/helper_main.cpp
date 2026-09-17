// SPDX-License-Identifier: MIT
/*
 * CEF subprocess helper
 *
 * Chromium is multi-process: the browser process (flutter-pi) spawns a zygote,
 * a GPU process and one renderer per site. On Linux those subprocesses are
 * launched by re-executing a binary, which is why they get their own tiny
 * executable instead of re-entering flutter-pi's main() -- a renderer that
 * accidentally booted a whole flutter engine would be a bad day.
 *
 * flutter-pi points CefSettings::browser_subprocess_path at this binary.
 *
 * Copyright (c) 2026, Bojidar Tonchev <bojidar.tonchev@gmail.com>
 */

#include "cef_bridge.h"

int main(int argc, char **argv) {
    const int exit_code = wvcef_execute_subprocess(argc, argv);

    // CefExecuteProcess returns -1 when the process isn't a CEF subprocess at
    // all, which for this binary means somebody ran it by hand.
    if (exit_code < 0) {
        return 1;
    }

    return exit_code;
}
