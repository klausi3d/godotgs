#include "gaussian_splat_dynamic_instance_3d.h"

#ifndef DISABLE_DEPRECATED

#include "core/error/error_macros.h"

GaussianSplatDynamicInstance3D::GaussianSplatDynamicInstance3D() {
    WARN_DEPRECATED_MSG(
            "GaussianSplatDynamicInstance3D is deprecated and will be removed in a "
            "future release. It now forwards verbatim to GaussianSplatNode3D, which "
            "terminates at the same scene-director instance registration with strictly "
            "more features. Migrate by changing the node class to GaussianSplatNode3D "
            "(use `splat_asset` for imported assets, `set_splat_data()` for runtime "
            "splats). Re-saving the scene with the new class drops this compat shim.");
}

#endif // DISABLE_DEPRECATED
