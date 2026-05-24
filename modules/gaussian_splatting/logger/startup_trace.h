#ifndef GS_STARTUP_TRACE_H
#define GS_STARTUP_TRACE_H

#include "core/os/mutex.h"
#include "core/string/string_name.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"

#include <atomic>
#include <cstdint>

// Per-asset-open snapshot of accumulated phases, sealed when a subsequent
// begin_asset_open() arrives before the renderer has consumed the prior open.
// begin_usec captures the OS::get_ticks_usec() value at begin_asset_open()
// time so flush() can report end-to-end duration from open boundary to first
// rendered frame, not just frame-entry time.
struct GSStartupTraceSnapshot {
	HashMap<StringName, uint64_t> totals_usec;
	LocalVector<StringName> insertion_order;
	uint64_t begin_usec = 0;
};

// Startup-time instrumentation that carries module-init phases into the first
// asset-open, then resets on later asset-open begins after a flush.
class GSStartupTrace {
public:
	static GSStartupTrace *get_singleton();

	static bool is_enabled_fast();

	// Project-setting gate. Refreshed explicitly during module init and at
	// asset-open boundaries so the macro can stay cheap.
	bool is_enabled() const { return is_enabled_fast(); }
	void refresh_enabled();

	// Starts a new asset-open trace after the previous one has flushed. If the
	// module has already recorded pre-open startup phases for the first asset,
	// they are preserved until that first flush. Arms a pending flush so the
	// next rendered frame emits the accumulated [StartupTrace] line.
	void begin_asset_open();

	void reset();

	// Releases all StringNames cached in the phase totals/insertion-order
	// containers. Called at module unregister so phase names recorded via
	// GS_STARTUP_SCOPE (manager_construct, module_register,
	// device_request_primary, device_request_shared, ...) do not show up
	// in the engine's exit-time orphan StringName report. Differs from
	// reset() only in intent — the underlying clears are the same.
	void release_module_strings();

	void begin_scope(const StringName &p_phase);
	void end_scope(const StringName &p_phase, uint64_t p_start_usec);

	void record_subphase(const StringName &p_phase, uint64_t p_duration_usec);
	void record_subphase(const char *p_phase, uint64_t p_duration_usec);

	// Drains one pending begin_asset_open() event: decrement pending count,
	// pop the oldest sealed snapshot (or take the active accumulator), and
	// emit one [StartupTrace] line. Returns true if a pending event was
	// consumed (caller should keep looping to drain all pending). The
	// decrement and emission happen under the same lock as begin_asset_open()
	// so a concurrent begin cannot observe a count==0 window and skip
	// sealing the previous open's accumulator. total= is computed from the
	// drained snapshot's own begin_asset_open() timestamp.
	bool flush_one_pending();

private:
	GSStartupTrace();

	static std::atomic<bool> enabled;

	mutable Mutex state_mutex;
	// Counts begin_asset_open() calls that have not yet been flushed. A
	// boolean here would collapse multiple back-to-back opens into one line.
	std::atomic<uint32_t> pending_flush_count{ 0 };
	// Sealed snapshots of prior asset-opens that arrived before the renderer
	// drained them. Oldest-first; flush() pops index 0.
	LocalVector<GSStartupTraceSnapshot> sealed_traces;
	// Active accumulator for the currently-open asset (or pre-open module-init
	// phases when no asset open has happened yet).
	HashMap<StringName, uint64_t> totals_usec;
	LocalVector<StringName> insertion_order;
	uint64_t active_begin_usec = 0;
	// True once any flush has emitted. Used by begin_asset_open() to decide
	// whether non-empty active represents the special first-open case
	// (preserve pre-module-init phases) or stale phases recorded outside
	// any in-flight open (drop them so they do not leak into the next line).
	bool ever_flushed = false;
};

class GSStartupTraceScope {
public:
	GSStartupTraceScope(const char *p_phase);
	~GSStartupTraceScope();

private:
	StringName phase;
	uint64_t start_usec = 0;
	bool active = false;
};

#define GS_STARTUP_SCOPE_CONCAT_INNER(a, b) a##b
#define GS_STARTUP_SCOPE_CONCAT(a, b) GS_STARTUP_SCOPE_CONCAT_INNER(a, b)
// The macro always constructs a GSStartupTraceScope so the RAII timer lives
// for the enclosing block, not just the macro line. When the trace is
// disabled, nullptr is passed and the constructor short-circuits cheaply.
// The previous `if (...) {} else GSStartupTraceScope X(name)` form put the
// variable inside the else branch, so its destructor fired immediately after
// the macro statement and the scope measured nothing.
#define GS_STARTUP_SCOPE(name) \
	GSStartupTraceScope GS_STARTUP_SCOPE_CONCAT(_gs_startup_scope_, __LINE__)( \
			GSStartupTrace::is_enabled_fast() ? (name) : nullptr)

#endif // GS_STARTUP_TRACE_H
