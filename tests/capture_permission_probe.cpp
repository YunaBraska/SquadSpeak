#include <CoreGraphics/CoreGraphics.h>
#include <atomic>

namespace {
std::atomic<int> requests{0};
bool requestScreenAccess() { ++requests; return false; }
__attribute__((used, section("__DATA,__interpose")))
const struct { const void* replacement; const void* original; } intercept{
    reinterpret_cast<const void*>(&requestScreenAccess), reinterpret_cast<const void*>(&CGRequestScreenCaptureAccess)};
}

extern "C" int screenPermissionRequests() { return requests.load(); }
