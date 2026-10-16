#include <cstdlib>
#include <cstring>

// Read by rgEeCvtWS (ee_fpu.h) in the generated code. Set before main(),
// and so before any guest code runs.
extern const bool rgEeCvtRoundToNearest = [] {
    const char *mode = std::getenv("RG_EE_CVT");
    return mode != nullptr && std::strcmp(mode, "round") == 0;
}();
