#include "streaming_telemetry_adapter.h"
#include "core/string/string_name.h"
#include "core/variant/variant.h"

namespace {

static const StringName KEY_QUEUE_PRESSURE_ACTIVE("queue_pressure_active");
static const StringName KEY_QUEUE_PRESSURE_SOURCE("queue_pressure_source");
static const StringName KEY_QUEUE_PRESSURE_REASON("queue_pressure_reason");
static const StringName KEY_QUEUE_PRESSURE_BACKLOG_DEPTH("queue_pressure_backlog_depth");
static const StringName KEY_QUEUE_PRESSURE_TOTAL_PENDING_CHUNKS("queue_pressure_total_pending_chunks");
static const StringName KEY_QUEUE_PRESSURE_PACK_SOURCE_ACTIVE("queue_pressure_pack_source_active");
static const StringName KEY_QUEUE_PRESSURE_UPLOAD_SOURCE_ACTIVE("queue_pressure_upload_source_active");
static const StringName KEY_QUEUE_PRESSURE_SYNC_SOURCE_ACTIVE("queue_pressure_sync_source_active");

static void _apply_queue_pressure_common(Dictionary &r_snapshot,
        const StreamingTelemetryAdapter::QueuePressureSnapshot &p_snapshot) {
    r_snapshot[KEY_QUEUE_PRESSURE_ACTIVE] = p_snapshot.active;
    r_snapshot[KEY_QUEUE_PRESSURE_SOURCE] = p_snapshot.source;
    r_snapshot[KEY_QUEUE_PRESSURE_REASON] = p_snapshot.reason;
    r_snapshot[KEY_QUEUE_PRESSURE_BACKLOG_DEPTH] = static_cast<int64_t>(p_snapshot.backlog_depth);
    r_snapshot[KEY_QUEUE_PRESSURE_TOTAL_PENDING_CHUNKS] = static_cast<int64_t>(p_snapshot.total_pending_chunks);
    r_snapshot[KEY_QUEUE_PRESSURE_PACK_SOURCE_ACTIVE] = p_snapshot.pack_source_active;
    r_snapshot[KEY_QUEUE_PRESSURE_UPLOAD_SOURCE_ACTIVE] = p_snapshot.upload_source_active;
    r_snapshot[KEY_QUEUE_PRESSURE_SYNC_SOURCE_ACTIVE] = p_snapshot.sync_source_active;
}

} // namespace

void StreamingTelemetryAdapter::apply_queue_pressure_analytics(
        Dictionary &r_analytics_snapshot, const QueuePressureSnapshot &p_snapshot) {
    _apply_queue_pressure_common(r_analytics_snapshot, p_snapshot);
}

void StreamingTelemetryAdapter::apply_queue_pressure_diagnostics(
        Dictionary &r_diagnostics_snapshot, const QueuePressureSnapshot &p_snapshot) {
    _apply_queue_pressure_common(r_diagnostics_snapshot, p_snapshot);
}
