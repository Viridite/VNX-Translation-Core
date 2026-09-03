// Host test harness for JNI native-method symbol matching. This is the rule
// that decides whether a cocos2d-x game finds its render loop at all, so it is
// kept as a pure string function precisely so it can be checked here rather
// than guessed at from a log after the fact.
//
//   g++ -std=c++17 -I include test/jnisym/harness.cpp -o /tmp/jnisymtest && /tmp/jnisymtest
#include "compat/jnisym.h"
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const char* what) {
    if (ok) { g_pass++; printf("  ok   %s\n", what); }
    else    { g_fail++; printf("  FAIL %s\n", what); }
}

int main() {
    printf("jnisym\n");
    const char* R = "Cocos2dxRenderer";

    // The stock name — Hill Climb Racing's, and the only one that used to work.
    check(jnisym::matches("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender", R, "nativeRender"),
          "the stock org.cocos2dx.lib name matches");

    // The same engine built under the game's own package: what this exists for.
    check(jnisym::matches("Java_com_robtopx_geometryjump_Cocos2dxRenderer_nativeRender", R, "nativeRender"),
          "a game that repackaged the engine matches");
    check(jnisym::matches("Java_org_cocos2dx_cpp_Cocos2dxRenderer_nativeRender", R, "nativeRender"),
          "the org.cocos2dx.cpp spelling matches");

    // JNI escapes an underscore in a package name as _1, so a package with one
    // in it is still unambiguous.
    check(jnisym::matches("Java_com_square_1enix_lib_Cocos2dxRenderer_nativeRender", R, "nativeRender"),
          "a package containing an escaped underscore matches");

    // An overloaded native carries its signature after a double underscore.
    check(jnisym::matches("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender__", R, "nativeRender"),
          "the overloaded long form matches");
    check(jnisym::matches("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesBegin__IFF",
                          R, "nativeTouchesBegin"),
          "an overload with a signature matches");

    // Wrong method, wrong class, and near-misses that must not match — a false
    // positive here is a call into the wrong function with the wrong arguments,
    // which is worse than finding nothing.
    check(!jnisym::matches("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit", R, "nativeRender"),
          "a different method on the right class does not match");
    check(!jnisym::matches("Java_org_cocos2dx_lib_Cocos2dxHelper_nativeRender", R, "nativeRender"),
          "the right method on a different class does not match");
    check(!jnisym::matches("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRenderTwice", R, "nativeRender"),
          "a longer method name with the same prefix does not match");
    check(!jnisym::matches("_ZN5cocos2d8RendererL12nativeRenderEv", R, "nativeRender"),
          "a C++ mangled internal symbol is not a JNI export");
    check(!jnisym::matches("nativeRender", R, "nativeRender"),
          "a bare name with no Java_ prefix does not match");

    // A class name appearing inside the package as well as at the end: the
    // match must anchor on the last occurrence, not the first.
    check(jnisym::matches("Java_com_Cocos2dxRenderer_pkg_Cocos2dxRenderer_nativeRender",
                          R, "nativeRender"),
          "the class name repeated in the package still matches at the end");

    check(!jnisym::matches(nullptr, R, "nativeRender") &&
          !jnisym::matches("Java_x_Cocos2dxRenderer_nativeRender", nullptr, "nativeRender") &&
          !jnisym::matches("Java_x_Cocos2dxRenderer_nativeRender", R, nullptr),
          "null arguments are handled");

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
