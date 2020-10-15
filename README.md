# vperfetto

vperfetto is a way to make it easier to process perfetto traces in the virtual machine use case. It consists of an online component where the user can set up guest host communication and send messages to the host and call into the vperfetto library to mark guest time and start/end of tracing, and an offline component to merge guest/host traces after the fact.

# Build

    mkdir build
    cd build
    cmake . ../
    make

generates `libvperfetto.so` for online and `vperfetto_merge` for offline usage.

# How to use

## Online using guest-triggered tracing

Link `libvperfetto.so`

Workflow (namespace vperfetto)

![vperfetto workflow](vperfetto-workflow.png)

## Offline using separate guest/host traces

This is useful if you've generated traces already but just want to merge them. The binary takes 3 mandatory arguments for the guest/host trace and another argument for the combined output trace file. There is one optional argument to specify the `CLOCK_BOOTTIME` in the guest (in nanoseconds) when the host trace started to help line things up:

`./vperfetto_merge <guest.trace> <host.trace> <combined.trace(forWriting)> [guestTraceStartTimeNs]`


# Min option

If you only need track events and counters, set `OPTION_USE_PERFETTO_SDK` to
`FALSE` in CMakeLists.txt; resulting binary is smaller.

# Known issues

Trace counters not yet supported in the SDK variant.
`vperfetto_merge` must be built with `OPTION_USE_PERFETTO_SDK` `TRUE` or it is useless.

# Library structure

`perfetto-min/` contains perfetto base libraries, protozero, and protobuf definitions. It's used to generate the non-SDK variant (`OPTION_USE_PERFETTO_SDK=FALSE`).

`proto/` is for the SDK variant (`OPTION_USE_PERFETTO_SDK=TRUE`) and contains the full `perfetto_trace.proto`.

`perfetto.cc/perfetto.h` is the v7 perfetto sdk, except also exporting a function to generate UUID lsbs for easier renaming of uuids in trace processing.

`vperfetto.h` is the interface to this library.

`vperfetto-sdk.cpp` is the perfetto SDK-based implementation of the interface (`OPTION_USE_PERFETTO_SDK=TRUE`).
`vperfetto.cpp` is the non-SDK implementation of the interface (`OPTION_USE_PERFETTO_SDK=FALSE`).

`vperfetto_unittest.cpp` contains tests. TODO: Add more
