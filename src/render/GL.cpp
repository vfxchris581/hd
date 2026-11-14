#include "GL.h"

#define HD_GL_DEFINE(ret, name, args) PFN_hd_##name hd_##name = nullptr;
HD_GL_FUNCTIONS(HD_GL_DEFINE)
#undef HD_GL_DEFINE

namespace hd {

bool loadGL(GLGetProc getProc, const char** missing) {
    bool ok = true;
#define HD_GL_LOAD(ret, name, args)                                              \
    hd_##name = reinterpret_cast<PFN_hd_##name>(getProc(#name));                 \
    if (!hd_##name && ok) { ok = false; if (missing) *missing = #name; }
    HD_GL_FUNCTIONS(HD_GL_LOAD)
#undef HD_GL_LOAD
    return ok;
}

} // namespace hd
