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
		active_begin_usec = 0;
		ever_flushed = false;
		return;
	}

	if (!insertion_order.is_empty()) {
		if (pending_flush_count.load(std::memory_order_relaxed) > 0) {
			// A prior begin_asset_open() has not yet been drained. Seal the
			// active accumulator as that prior open's snapshot so this new
			// open starts with a clean slate instead of merging.
			GSStartupTraceSnapshot snapshot;
			snapshot.totals_usec = totals_usec;
			snapshot.insertion_order = insertion_order;
			snapshot.begin_usec = active_begin_usec;
			sealed_traces.push_back(snapshot);
			totals_usec.clear();
			insertion_order.clear();
		} else if (ever_flushed) {
			// count==0 means the prior open already flushed; any phases that
			// have accumulated since (for example a GS_STARTUP_SCOPE that
			// fired outside an asset-open boundary) are stale and would
			// otherwise be attributed to this new open. Drop them.
			totals_usec.clear();
			insertion_order.clear();
		}
		// else (count==0 && !ever_flushed): first-ever begin with pre-open
		// module-init phases in active. Preserve so the first
		// [StartupTrace] line includes them.
	}

	OS *os = OS::get_singleton();
	active_begin_usec = os ? os->get_ticks_usec() : 0;

	pending_flush_count.fetch_add(1, std::memory_order_acq_rel);
}

void GSStartupTrace::reset() {
	MutexLock lock(state_mutex);
	sealed_traces.clear();
	totals_usec.clear();
	insertion_order.clear();
	pending_flush_count.store(0, std::memory_order_relaxed);
	active_begin_usec = 0;
	ever_flushed = false;
}

// flush() and consume_pending_flush() used to be separate operations, but
// decrementing pending_flush_count outside the state_mutex creates a window in
// which a concurrent begin_asset_open() observes count==0 and skips sealing
// the previous open's accumulator. flush_one_pending() now performs the
// decrement, snapshot pop, and emission all under the same mutex so begin's
// seal check is consistent with the live count.

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

bool GSStartupTrace::flush_one_pending() {
	String line;
	bool emitted = false;
	bool consumed = false;
	{
		MutexLock lock(state_mutex);
		if (pending_flush_count.load(std::memory_order_relaxed) == 0) {
			return false;
		}

		// If the trace was disabled between begin_asset_open() and now, fully
		// reset accumulator state so a later re-enable does not merge stale
		// phases from this disabled window into a new asset-open line.
		// Zeroing the count here makes the caller's drain loop exit on the
		// next iteration; subsequent toggles start from a clean slate.
		if (!is_enabled_fast()) {
			sealed_traces.clear();
			totals_usec.clear();
			insertion_order.clear();
			active_begin_usec = 0;
			ever_flushed = false;
			pending_flush_count.store(0, std::memory_order_release);
			return false;
		}

		// Drain the oldest sealed snapshot first so multiple back-to-back
		// asset opens each emit their own line in arrival order. When no
		// sealed snapshot is queued, emit the active accumulator and clear
		// it so the next open starts fresh.
		const HashMap<StringName, uint64_t> *emit_totals = nullptr;
		const LocalVector<StringName> *emit_order = nullptr;
		uint64_t emit_begin_usec = 0;
		GSStartupTraceSnapshot popped;
		if (!sealed_traces.is_empty()) {
			popped = sealed_traces[0];
			sealed_traces.remove_at(0);
			emit_totals = &popped.totals_usec;
			emit_order = &popped.insertion_order;
			emit_begin_usec = popped.begin_usec;
		} else if (!insertion_order.is_empty()) {
			emit_totals = &totals_usec;
			emit_order = &insertion_order;
			emit_begin_usec = active_begin_usec;
		}

		// Decrement under the lock so a concurrent begin_asset_open() either
		// sees count>0 (and seals the previous accumulator correctly) or
		// sees the post-emit state where active has already been cleared.
		pending_flush_count.fetch_sub(1, std::memory_order_acq_rel);
		consumed = true;

		if (emit_totals == nullptr) {
			// Pending event with no content to emit (begin_asset_open() fired
			// before any scope recorded a phase). Decrement the count so the
			// caller's drain loop can move on; nothing else to do.
			return true;
		}

		// total= measures from begin_asset_open() to now so it reflects the
		// end-to-end startup duration the user actually sees, not the
		// frame-entry slice the caller happens to be inside.
		OS *os = OS::get_singleton();
		const uint64_t now_usec = os ? os->get_ticks_usec() : 0;
		const double total_ms = (emit_begin_usec > 0 && now_usec >= emit_begin_usec)
				? double(now_usec - emit_begin_usec) / 1000.0
				: 0.0;

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
		line += String::num(total_ms, 2);
		line += "ms";

		// Only clear the active accumulator after we have actually emitted
		// from it (snapshots own their copy). This keeps active empty for
		// the next open's phases.
		if (emit_totals == &totals_usec) {
			totals_usec.clear();
			insertion_order.clear();
			active_begin_usec = 0;
		}
		emitted = true;
		ever_flushed = true;
	}

	if (emitted) {
		GS_LOG_RENDERER_INFO(line);
	}
	return consumed;
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
