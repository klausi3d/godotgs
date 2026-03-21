#include "debug_overlay_system.h"
#include "debug_overlay_macros.h"
#include "core/math/math_funcs.h"
#include "core/string/ustring.h"
#include "servers/rendering_server.h"
#include "../core/gaussian_splat_manager.h"
#include "../renderer/gaussian_splat_renderer.h"
#include "gpu_sorting_pipeline.h"

void DebugOverlaySystem::_bind_methods() {
    // Bind methods for script access if needed
}

DebugOverlaySystem::DebugOverlaySystem() {
}

DebugOverlaySystem::~DebugOverlaySystem() {
    shutdown();
}

void DebugOverlaySystem::initialize() {
    options = DebugOverlayOptions();
    counters = DebugCounterSnapshot();
    binning_counters.clear();
    tile_density_cache.clear();
    tile_density_width = 0;
    tile_density_height = 0;
    tile_density_peak = 0;
    tile_density_average = 0.0f;
    dirty = false;
    version = 0;
}

void DebugOverlaySystem::shutdown() {
    binning_counters.clear();
    tile_density_cache.clear();
}

void DebugOverlaySystem::set_options(const DebugOverlayOptions &p_options) {
    options = p_options;
    _mark_dirty();
}

DebugOverlayOptions DebugOverlaySystem::get_options() const {
    return options;
}

// Standard boolean setters - use macros to reduce boilerplate
GS_DEBUG_OVERLAY_SETTER_IMPL(show_tile_bounds)
GS_DEBUG_OVERLAY_SETTER_IMPL(show_splat_coverage)
GS_DEBUG_OVERLAY_SETTER_IMPL(show_tile_grid)
GS_DEBUG_OVERLAY_SETTER_IMPL(show_overflow_tiles)
GS_DEBUG_OVERLAY_SETTER_IMPL(show_projection_issues)
GS_DEBUG_OVERLAY_SETTER_IMPL(show_white_albedo)
GS_DEBUG_OVERLAY_SETTER_IMPL(show_density_heatmap)
GS_DEBUG_OVERLAY_SETTER_IMPL(show_shadow_opacity)

// Mutually exclusive setters - resolve_input and resolve_output cannot both be enabled
GS_DEBUG_OVERLAY_SETTER_EXCLUSIVE_IMPL(show_resolve_input, show_resolve_output)
GS_DEBUG_OVERLAY_SETTER_EXCLUSIVE_IMPL(show_resolve_output, show_resolve_input)

GS_DEBUG_OVERLAY_SETTER_IMPL(show_performance_hud)
GS_DEBUG_OVERLAY_SETTER_IMPL(show_residency_hud)
GS_DEBUG_OVERLAY_SETTER_IMPL(show_device_boundaries)
GS_DEBUG_OVERLAY_SETTER_IMPL(show_texture_states)

void DebugOverlaySystem::set_overlay_opacity(float p_opacity) {
    p_opacity = CLAMP(p_opacity, 0.0f, 1.0f);
    if (options.overlay_opacity != p_opacity) {
        options.overlay_opacity = p_opacity;
        _mark_dirty();
    }
}

GS_DEBUG_OVERLAY_SETTER_IMPL(dump_gpu_counters)

DebugCounterSnapshot DebugOverlaySystem::get_debug_counters() const {
    return counters;
}

Dictionary DebugOverlaySystem::get_binning_debug_counters() const {
    return binning_counters;
}

void DebugOverlaySystem::reset_counters() {
    counters = DebugCounterSnapshot();
    binning_counters.clear();
}

bool DebugOverlaySystem::has_active_overlays() const {
    return options.show_tile_bounds ||
           options.show_splat_coverage ||
           options.show_tile_grid ||
           options.show_overflow_tiles ||
           options.show_projection_issues ||
           options.show_white_albedo ||
           options.show_density_heatmap ||
           options.show_shadow_opacity ||
           options.show_resolve_input ||
           options.show_resolve_output ||
           options.show_performance_hud ||
           options.show_residency_hud ||
           options.show_device_boundaries ||
           options.show_texture_states;
}

void DebugOverlaySystem::update_counters(const DebugCounterSnapshot &p_counters) {
    counters = p_counters;
}

void DebugOverlaySystem::update_binning_counters(const Dictionary &p_counters) {
    binning_counters = p_counters;
}

void DebugOverlaySystem::_mark_dirty() {
    dirty = true;
    version++;
}

// HUD building and overlay statistics - extracted from GaussianSplatRenderer god class

void DebugOverlaySystem::rebuild_overlay_statistics_from_tile_density() {
    if (!options.show_tile_grid && !options.show_density_heatmap) {
        tile_density_peak = 0;
        tile_density_average = 0.0f;
        return;
    }

    if (tile_density_cache.is_empty()) {
        tile_density_peak = 0;
        tile_density_average = 0.0f;
        return;
    }

    const uint32_t *density_ptr = tile_density_cache.ptr();
    uint64_t total = 0;
    uint32_t peak = 0;
    uint32_t non_zero_tiles = 0;

    const int density_count = tile_density_cache.size();
    for (int i = 0; i < density_count; i++) {
        uint32_t value = density_ptr[i];
        peak = MAX(peak, value);
        if (value > 0) {
            total += value;
            non_zero_tiles++;
        }
    }

    tile_density_peak = peak;
    tile_density_average = non_zero_tiles > 0 ? float(total) / float(non_zero_tiles) : 0.0f;
}

void DebugOverlaySystem::update_tile_density_cache(const Vector<uint32_t> &p_tile_counts,
        const Vector2i &p_tile_grid, uint32_t p_peak, float p_average) {
    tile_density_cache = p_tile_counts;
    tile_density_width = p_tile_grid.x;
    tile_density_height = p_tile_grid.y;
    tile_density_peak = p_peak;
    tile_density_average = p_average;
}

void DebugOverlaySystem::clear_tile_density_cache() {
    tile_density_cache.clear();
    tile_density_width = 0;
    tile_density_height = 0;
    tile_density_peak = 0;
    tile_density_average = 0.0f;
}

// Renderer-syncing setters - overlay invalidation variants
GS_DEBUG_OVERLAY_RENDERER_SETTER_OVERLAY_IMPL(show_tile_grid)
GS_DEBUG_OVERLAY_RENDERER_SETTER_OVERLAY_IMPL(show_density_heatmap)
GS_DEBUG_OVERLAY_RENDERER_SETTER_OVERLAY_IMPL(show_device_boundaries)
GS_DEBUG_OVERLAY_RENDERER_SETTER_OVERLAY_IMPL(show_texture_states)

// Renderer-syncing setters - performance_hud and residency_hud use overlay invalidation
// (HUD node was removed; the canonical overlay is now GDScript-based)
GS_DEBUG_OVERLAY_RENDERER_SETTER_OVERLAY_IMPL(show_performance_hud)
GS_DEBUG_OVERLAY_RENDERER_SETTER_OVERLAY_IMPL(show_residency_hud)

void DebugOverlaySystem::set_renderer_overlay_opacity(GaussianSplatRenderer *p_renderer, float p_opacity) {
    if (!p_renderer) {
        return;
    }

    float clamped = CLAMP(p_opacity, 0.0f, 1.0f);
    auto &debug_config = p_renderer->get_debug_config();
    if (Math::is_equal_approx(debug_config.overlay_opacity, clamped)) {
        return;
    }

    debug_config.overlay_opacity = clamped;
    set_overlay_opacity(clamped);
    invalidate_renderer_overlay(p_renderer, true);
}

void DebugOverlaySystem::invalidate_renderer_overlay(GaussianSplatRenderer *p_renderer, bool p_increment_version) {
    if (!p_renderer) {
        return;
    }

    auto &debug_state = p_renderer->get_debug_state();
    if (p_increment_version) {
        debug_state.overlay_version++;
    }

    debug_state.overlay_dirty = true;

    if (!debug_state.show_tile_grid && !debug_state.show_density_heatmap) {
        debug_state.tile_density_cache.clear();
        debug_state.tile_density_width = 0;
        debug_state.tile_density_height = 0;
        debug_state.tile_density_peak = 0;
        debug_state.tile_density_average = 0.0f;
    }
}

void DebugOverlaySystem::rebuild_renderer_overlay_statistics_from_cache(GaussianSplatRenderer *p_renderer) {
    if (!p_renderer) {
        return;
    }

    auto &debug_state = p_renderer->get_debug_state();
    if (!debug_state.show_tile_grid && !debug_state.show_density_heatmap) {
        debug_state.overlay_dirty = false;
        debug_state.tile_density_peak = 0;
        debug_state.tile_density_average = 0.0f;
        return;
    }

    if (debug_state.tile_density_cache.is_empty()) {
        debug_state.overlay_dirty = false;
        debug_state.tile_density_peak = 0;
        debug_state.tile_density_average = 0.0f;
        return;
    }

    const uint32_t *density_ptr = debug_state.tile_density_cache.ptr();
    uint64_t total = 0;
    uint32_t peak = 0;
    uint32_t non_zero_tiles = 0;

    const int density_count = debug_state.tile_density_cache.size();
    for (int i = 0; i < density_count; i++) {
        uint32_t value = density_ptr[i];
        peak = MAX(peak, value);
        if (value > 0) {
            total += value;
            non_zero_tiles++;
        }
    }

    debug_state.tile_density_peak = peak;
    debug_state.tile_density_average = non_zero_tiles > 0 ? float(total) / float(non_zero_tiles) : 0.0f;
    debug_state.overlay_dirty = false;
}

