#ifndef GS_SORTING_SETTINGS_UTILS_H
#define GS_SORTING_SETTINGS_UTILS_H

#include "core/config/project_settings.h"
#include "../core/gs_project_settings.h"

#include "core/error/error_macros.h"
#include "core/string/string_name.h"

namespace gs {
namespace sorting_settings {

struct TargetSortTimeObservation {
	bool initialized = false;
	bool last_present = false;
	float last_value = 0.0f;
	bool builtin_runtime_override_active = false;
};

static inline const StringName &target_sort_time_path() {
	static const StringName path("rendering/gaussian_splatting/sorting/target_sort_time_ms");
	return path;
}

static inline const StringName &legacy_gpu_target_sort_time_path() {
	static const StringName path("rendering/gaussian_splatting/gpu_sorting/target_sort_time_ms");
	return path;
}

static inline TargetSortTimeObservation &target_sort_time_observation() {
	static TargetSortTimeObservation observation;
	return observation;
}

static inline void reset_target_sort_time_observation(ProjectSettings *p_ps) {
	TargetSortTimeObservation &observation = target_sort_time_observation();
	observation = TargetSortTimeObservation();
	if (!p_ps) {
		return;
	}
	observation.initialized = true;
	observation.last_present = p_ps->has_setting(String(target_sort_time_path()));
	if (observation.last_present) {
		observation.last_value = gs::settings::get_float(p_ps, target_sort_time_path(), 0.0f);
	}
}

static inline void sync_target_sort_time_observation(ProjectSettings *p_ps) {
	TargetSortTimeObservation &observation = target_sort_time_observation();
	if (!p_ps) {
		observation = TargetSortTimeObservation();
		return;
	}
	const String path = String(target_sort_time_path());
	const bool has_canonical = p_ps->has_setting(path);
	const float current_value = has_canonical ? gs::settings::get_float(p_ps, target_sort_time_path(), 0.0f) : 0.0f;
	if (!observation.initialized) {
		observation.initialized = true;
		observation.last_present = has_canonical;
		observation.last_value = current_value;
		return;
	}
	if (!has_canonical) {
		observation.last_present = false;
		observation.last_value = 0.0f;
		observation.builtin_runtime_override_active = false;
		return;
	}
	if (observation.last_present && p_ps->is_builtin_setting(path) && !Math::is_equal_approx(current_value, observation.last_value)) {
		// ProjectSettings keeps builtin order on runtime writes, so remember that a
		// builtin-valued canonical key was edited during this session.
		observation.builtin_runtime_override_active = true;
	}
	observation.last_present = true;
	observation.last_value = current_value;
}

static inline bool has_explicit_target_sort_time_override(ProjectSettings *p_ps) {
	if (!p_ps) {
		return false;
	}
	sync_target_sort_time_observation(p_ps);
	if (!p_ps->has_setting(String(target_sort_time_path()))) {
		return false;
	}
	if (!p_ps->is_builtin_setting(String(target_sort_time_path()))) {
		return true;
	}
	if (target_sort_time_observation().builtin_runtime_override_active) {
		return true;
	}
	if (!p_ps->property_can_revert(target_sort_time_path())) {
		return true;
	}
	return p_ps->get_setting_with_override(target_sort_time_path()) != p_ps->property_get_revert(target_sort_time_path());
}

static inline void register_canonical_target_sort_time_setting(ProjectSettings *p_ps, float p_default_value, bool p_preserve_runtime_observation = false) {
	if (p_preserve_runtime_observation) {
		sync_target_sort_time_observation(p_ps);
	}
	const bool preserve_runtime_override = p_preserve_runtime_observation && target_sort_time_observation().builtin_runtime_override_active;
	const bool had_project_target_override = p_ps && p_ps->has_setting(String(target_sort_time_path())) &&
			!p_ps->is_builtin_setting(String(target_sort_time_path()));
	const int prior_target_order = had_project_target_override ? p_ps->get_order(target_sort_time_path()) : -1;
	GLOBAL_DEF(String(target_sort_time_path()), p_default_value);
	if (had_project_target_override) {
		p_ps->set_order(target_sort_time_path(), prior_target_order);
	}
	reset_target_sort_time_observation(p_ps);
	target_sort_time_observation().builtin_runtime_override_active = preserve_runtime_override;
}

static inline float get_target_sort_time_ms(ProjectSettings *p_ps, float p_fallback) {
	if (!p_ps) {
		return p_fallback;
	}
	if (has_explicit_target_sort_time_override(p_ps)) {
		return gs::settings::get_float(p_ps, target_sort_time_path(), p_fallback);
	}
	if (p_ps->has_setting(legacy_gpu_target_sort_time_path())) {
		WARN_PRINT_ONCE(vformat("[GaussianSplatting] Project setting '%s' is deprecated; use '%s' instead. Legacy alias support is read-only compatibility and will be removed after project migration.",
				String(legacy_gpu_target_sort_time_path()),
				String(target_sort_time_path())));
		return gs::settings::get_float(p_ps, legacy_gpu_target_sort_time_path(), p_fallback);
	}
	if (p_ps->has_setting(String(target_sort_time_path()))) {
		return gs::settings::get_float(p_ps, target_sort_time_path(), p_fallback);
	}
	return p_fallback;
}

} // namespace sorting_settings
} // namespace gs

#endif // GS_SORTING_SETTINGS_UTILS_H
