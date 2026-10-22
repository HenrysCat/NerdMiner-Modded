// The firmware files under test, compiled for the host with the stand-ins of
// test/native_shim (see [env:native] in platformio.ini).
#include "../native_shim/host_shim.cpp"
#include "../../src/stratum.cpp"
#include "../../src/utils.cpp"
#include "../../src/ShaTests/nerdSHA256plus.cpp"
