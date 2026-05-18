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
struct GSStartupTraceSnapshot {
	HashMap<StringName, uint64_t> totals_usec;
	LocalVector<StringName> insertion_order;
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

	void begin_scope(const StringName &p_phase);
	void end_scope(const StringName &p_phase, uint64_t p_start_usec);

	void record_subphase(const StringName &p_phase, uint64_t p_duration_usec);
	void record_subphase(const char *p_phase, uint64_t p_duration_usec);

	void flush(double p_total_ms);

	// Atomically decrements the pending-flush counter if positive. Returns true
	// exactly once per begin_asset_open() call so multiple opens that happen
	// before the renderer drains them each get their own [StartupTrace] line.
	bool consume_pending_flush();

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
#define GS_STARTUP_SCOPE(name) \
	if (!GSStartupTrace::is_enabled_fast()) { \
	} else \
		GSStartupTraceScope GS_STARTUP_SCOPE_CONCAT(_gs_startup_scope_, __LINE__)(name)

#endif // GS_STARTUP_TRACE_H
