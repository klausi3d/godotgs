#ifndef GS_SORTING_SETTINGS_UTILS_H
#define GS_SORTING_SETTINGS_UTILS_H

#include "../core/gs_project_settings.h"

#include "core/error/error_macros.h"
#include "core/string/string_name.h"

namespace gs {
namespace sorting_settings {

static inline const StringName &target_sort_time_path() {
	static const StringName path("rendering/gaussian_splatting/sorting/target_sort_time_ms");
	return path;
}

static inline const StringName &legacy_gpu_target_sort_time_path() {
	static const StringName path("rendering/gaussian_splatting/gpu_sorting/target_sort_time_ms");
	return path;
}

static inline float get_target_sort_time_ms(ProjectSettings *p_ps, float p_fallback) {
	if (!p_ps) {
		return p_fallback;
	}
	if (p_ps->has_setting(target_sort_time_path())) {
		return gs::settings::get_float(p_ps, target_sort_time_path(), p_fallback);
	}
	if (p_ps->has_setting(legacy_gpu_target_sort_time_path())) {
		WARN_PRINT_ONCE(vformat("[GaussianSplatting] Project setting '%s' is deprecated; use '%s' instead. Legacy alias support is read-only compatibility and will be removed after project migration.",
				String(legacy_gpu_target_sort_time_path()),
				String(target_sort_time_path())));
		return gs::settings::get_float(p_ps, legacy_gpu_target_sort_time_path(), p_fallback);
	}
	return p_fallback;
}

} // namespace sorting_settings
} // namespace gs

#endif // GS_SORTING_SETTINGS_UTILS_H
