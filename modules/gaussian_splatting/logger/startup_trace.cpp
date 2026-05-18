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
		sealed_traces.clear();
		totals_usec.clear();
		insertion_order.clear();
		pending_flush_count.store(0, std::memory_order_relaxed);
		return;
	}

	// If a prior begin_asset_open() has not yet been drained by the renderer,
	// seal the active accumulator as that prior open's snapshot before
	// starting the new one. This keeps each open's timings in its own line
	// instead of merging multiple opens into one.
	if (pending_flush_count.load(std::memory_order_relaxed) > 0 && !insertion_order.is_empty()) {
		GSStartupTraceSnapshot snapshot;
		snapshot.totals_usec = totals_usec;
		snapshot.insertion_order = insertion_order;
		sealed_traces.push_back(snapshot);
		totals_usec.clear();
		insertion_order.clear();
	}
	// First call (pending_flush_count == 0) preserves any pre-open module-init
	// phases already recorded in the active accumulator so the first
	// [StartupTrace] line includes them.

	pending_flush_count.fetch_add(1, std::memory_order_acq_rel);
}

void GSStartupTrace::reset() {
	MutexLock lock(state_mutex);
	sealed_traces.clear();
	totals_usec.clear();
	insertion_order.clear();
	pending_flush_count.store(0, std::memory_order_relaxed);
}

bool GSStartupTrace::consume_pending_flush() {
	// Atomic decrement-if-positive: CAS loop guards against two consumers
	// racing to drain the same begin_asset_open() event.
	uint32_t expected = pending_flush_count.load(std::memory_order_acquire);
	while (expected > 0) {
		if (pending_flush_count.compare_exchange_weak(expected, expected - 1,
					std::memory_order_acq_rel, std::memory_order_acquire)) {
			return true;
		}
	}
	return false;
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
		if (!is_enabled_fast()) {
			return;
		}

		// Drain the oldest sealed snapshot first so multiple back-to-back
		// asset opens each emit their own line in arrival order. When no
		// sealed snapshot is queued, emit the active accumulator and clear
		// it so the next open starts fresh.
		const HashMap<StringName, uint64_t> *emit_totals = nullptr;
		const LocalVector<StringName> *emit_order = nullptr;
		GSStartupTraceSnapshot popped;
		if (!sealed_traces.is_empty()) {
			popped = sealed_traces[0];
			sealed_traces.remove_at(0);
			emit_totals = &popped.totals_usec;
			emit_order = &popped.insertion_order;
		} else if (!insertion_order.is_empty()) {
			emit_totals = &totals_usec;
			emit_order = &insertion_order;
		} else {
			return;
		}

		line = "[StartupTrace]";
		for (const StringName &phase : *emit_order) {
			HashMap<StringName, uint64_t>::ConstIterator it = emit_totals->find(phase);
			if (it == emit_totals->end()) {
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

		// Only clear the active accumulator after we have actually emitted
		// from it (snapshots own their copy). This keeps active empty for
		// the next open's phases.
		if (emit_totals == &totals_usec) {
			totals_usec.clear();
			insertion_order.clear();
		}
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
