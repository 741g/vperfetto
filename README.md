# Build

    mkdir build
    cd build
    cmake . ../
    make

generates `libvperfetto.so`

# How to use

Link `libvperfetto.so`

Workflow (namespace vperfetto)

![vperfetto workflow](vperfetto-workflow.png)
    
# Min option

If you only need track events and counters, set `OPTION_USE_PERFETTO_SDK` to
`FALSE` in CMakeLists.txt; resulting binary is smaller.

# Known issues

Trace counters not yet supported in the SDK variant.

# Library structure

`perfetto-min/` contains perfetto base libraries, protozero, and protobuf definitions. It's used to generate the non-SDK variant (`OPTION_USE_PERFETTO_SDK=FALSE`).

`proto/` is for the SDK variant (`OPTION_USE_PERFETTO_SDK=TRUE`) and contains the full `perfetto_trace.proto`.

`perfetto.cc/perfetto.h` is the v7 perfetto sdk, except also exporting a function to generate UUID lsbs for easier renaming of uuids in trace processing.

`vperfetto.h` is the interface to this library.

`vperfetto-sdk.cpp` is the perfetto SDK-based implementation of the interface (`OPTION_USE_PERFETTO_SDK=TRUE`).
`vperfetto.cpp` is the non-SDK implementation of the interface (`OPTION_USE_PERFETTO_SDK=FALSE`).

`vperfetto_unittest.cpp` contains tests. TODO: Add more
