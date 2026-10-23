// The firmware files under test, compiled for the host with the stand-ins of
// test/native_shim (see [env:native] in platformio.ini).
#include "../native_shim/host_shim.cpp"
#include "../../src/stratum.cpp"
#include "../../src/utils.cpp"
#include "../../src/ShaTests/nerdSHA256plus.cpp"
// The settings file (config save / load). It includes the board header: any
// board will do for these tests, this one has no screen to stub out.
#define DEVKITV1
#include "../../src/drivers/storage/nvMemory.cpp"
