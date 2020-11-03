// Copyright 2020 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "vperfetto-min.h"

#include <gtest/gtest.h>

#ifdef _WIN32
#include <windows.h>
#undef ERROR
#include <errno.h>
#include <stdio.h>
#endif

#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef _MSC_VER
#include <unistd.h>
#endif

#include <cstdio>
#include <filesystem>

static void runTrace(const vperfetto_min_config* config, uint32_t iterations) {
    vperfetto_min_startTracing(config);
    for (uint32_t i = 0; i < iterations; ++i) {
        vperfetto_min_beginTrackEvent("test trace 1");
        {
            vperfetto_min_beginTrackEvent("test trace 1.1");
            vperfetto_min_endTrackEvent();
        }
        vperfetto_min_endTrackEvent();
        vperfetto_min_beginTrackEvent_OpenGL("test OpenGL event");
        vperfetto_min_endTrackEvent_OpenGL();
    }
    vperfetto_min_endTracing();
}

TEST(VperfettoMin, Basic) {
    static char trace1FileName[L_tmpnam];

    if (!std::tmpnam(trace1FileName)) {
        FAIL() << "Could not generate trace1 file name";
        return;
    }

    fprintf(stderr, "%s: temp names: %s\n", __func__, trace1FileName);

    vperfetto_min_config config = {
        VPERFETTO_INIT_FLAG_USE_INPROCESS_BACKEND,
        trace1FileName,
    };
    runTrace(&config, 400);

    // std::filesystem::remove(std::filesystem::path(trace1FileName));
}
