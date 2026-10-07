#include <fontconfig/fontconfig.h>
#include <sanitizer/lsan_interface.h>
#include <cstdio>
#include <cstdlib>

namespace {
FcPattern* held = nullptr;
void* volatile canary = nullptr;
}

int main(int argc, char**) {
    if (argc > 1) {
        canary = std::malloc(31);
        canary = nullptr;
        return 0;
    }
    auto* request = FcNameParse(reinterpret_cast<const FcChar8*>("sans-serif"));
    if (!request) return 2;
    FcConfigSubstitute(nullptr, request, FcMatchPattern);
    FcDefaultSubstitute(request);
    FcResult result;
    held = FcFontMatch(nullptr, request, &result);
    FcPatternDestroy(request);
    if (!held) return 3;
    const int whileOwned = __lsan_do_recoverable_leak_check();
    FcChar8* family = nullptr;
    if (FcPatternGetString(held, FC_FAMILY, 0, &family) != FcResultMatch) return 4;
    std::fprintf(stderr, "Owned pattern remains readable: %s\n", family);
    FcPatternDestroy(held);
    held = nullptr;
    FcFini();
    const int afterRelease = __lsan_do_recoverable_leak_check();
    std::fprintf(stderr, "Fontconfig reachability: owned=%d released=%d\n", whileOwned, afterRelease);
    return afterRelease;
}
