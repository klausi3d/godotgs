#include "startup_trace.h"

#include "../core/gs_project_settings.h"
#include "gs_logger.h"

#include "core/config/project_settings.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/string/ustring.h"

namespace {
static constexpr int STARTUP_TRACE_RESERVE = 32;
static constexpr const char *STARTUP_TRACE_SETTING_PATH =
		"rendering/gaussian_splatting/diagnostics/startup_trace";
} // namespace

std::atomic<bool> GSStartupTrace::enabled{ false };

GSStartupTrace *GSStartupTrace::get_singleton() {
	static GSStartupTrace instance;
	return &instance;
}

GSStartupTrace::GSStartupTrace() {
	totals_usec.reserve(STARTUP_TRACE_RESERVE);
	insertion_order.reserve(STARTUP_TRACE_RESERVE);
}

bool GSStartupTrace::is_enabled_fast() {
	return enabled.load(std::memory_order_relaxed);
}

void GSStartupTrace::refresh_enabled() {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const bool value = gs::settings::get_bool(ps, StringName(STARTUP_TRACE_SETTING_PATH), true);
	enabled.store(value, std::memory_order_relaxed);
}

void GSStartupTrace::begin_asset_open() {
	refresh_enabled();

	MutexLock lock(state_mutex);
	if (!is_enabled_fast()) {
		totals_usec.clear();
		insertion_order.clear();
		flushed = false;
		pending_flush.store(false, std::memory_order_relaxed);
		return;
	}

	// Preserve any pre-open module-init phases recorded for the first asset
	// open. For subsequent asset opens (or after a flush), clear the
	// accumulator so each open emits its own trace line.
	if (flushed || insertion_order.is_empty()) {
		totals_usec.clear();
		insertion_order.clear();
		flushed = false;
	}
	pending_flush.store(true, std::memory_order_release);
}

void GSStartupTrace::reset() {
	MutexLock lock(state_mutex);
	totals_usec.clear();
	insertion_order.clear();
	flushed = false;
	pending_flush.store(false, std::memory_order_relaxed);
}

bool GSStartupTrace::consume_pending_flush() {
	return pending_flush.exchange(false, std::memory_order_acq_rel);
}

void GSStartupTrace::begin_scope(const StringName & /*p_phase*/) {
	// Scope start is captured by the RAII object itself; this hook is kept for
	// symmetry and future per-scope bookkeeping.
}

void GSStartupTrace::end_scope(const StringName &p_phase, uint64_t p_start_usec) {
	OS *os = OS::get_singleton();
	if (!os) {
		return;
	}
	const uint64_t now = os->get_ticks_usec();
	const uint64_t duration = now >= p_start_usec ? now - p_start_usec : 0;
	record_subphase(p_phase, duration);
}

void GSStartupTrace::record_subphase(const StringName &p_phase, uint64_t p_duration_usec) {
	if (!is_enabled_fast()) {
		return;
	}

	MutexLock lock(state_mutex);
	if (flushed) {
		return;
	}

	HashMap<StringName, uint64_t>::Iterator it = totals_usec.find(p_phase);
	if (it == totals_usec.end()) {
		totals_usec.insert(p_phase, p_duration_usec);
		insertion_order.push_back(p_phase);
	} else {
		it->value += p_duration_usec;
	}
}

void GSStartupTrace::record_subphase(const char *p_phase, uint64_t p_duration_usec) {
	if (!p_phase || !is_enabled_fast()) {
		return;
	}
	record_subphase(StringName(p_phase), p_duration_usec);
}

void GSStartupTrace::flush(double p_total_ms) {
	String line;
	{
		MutexLock lock(state_mutex);
		if (flushed) {
			return;
		}
		flushed = true;
		if (!is_enabled_fast() || insertion_order.is_empty()) {
			return;
		}

		line = "[StartupTrace]";
		for (const StringName &phase : insertion_order) {
			HashMap<StringName, uint64_t>::ConstIterator it = totals_usec.find(phase);
			if (it == totals_usec.end()) {
				continue;
			}
			line += " ";
			line += String(phase);
			line += "=";
			line += String::num(double(it->value) / 1000.0, 2);
			line += "ms";
		}
		line += " total=";
		line += String::num(p_total_ms, 2);
		line += "ms";
	}

	GS_LOG_RENDERER_INFO(line);
}

GSStartupTraceScope::GSStartupTraceScope(const char *p_phase) {
	if (!p_phase || !GSStartupTrace::is_enabled_fast()) {
		return;
	}

	GSStartupTrace *trace = GSStartupTrace::get_singleton();
	if (!trace) {
		return;
	}

	OS *os = OS::get_singleton();
	if (!os) {
		return;
	}

	phase = StringName(p_phase);
	start_usec = os->get_ticks_usec();
	active = true;
	trace->begin_scope(phase);
}

GSStartupTraceScope::~GSStartupTraceScope() {
	if (!active) {
		return;
	}
	GSStartupTrace *trace = GSStartupTrace::get_singleton();
	if (!trace) {
		return;
	}
	trace->end_scope(phase, start_usec);
}
