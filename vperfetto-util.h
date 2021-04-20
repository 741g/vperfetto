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
#include <cstdint>

// Assumes that the difference is less than abs(INT64_MAX).
static inline int64_t getSignedDifference(uint64_t a, uint64_t b) {
    uint64_t absDiff = (a > b) ? a - b : b - a;
    absDiff = (absDiff < INT64_MAX) ? absDiff : INT64_MAX;
    return (a > b) ? (int64_t)absDiff : -(int64_t)absDiff;
}
