#include "support.h"

int main(void)
{
    reset();
    join(10);
    chat_sessions_start_heartbeat(&ctx);
    sends = 0;
    now_s = HEARTBEAT_INTERVAL_S;
    schedule_sweep();
    assert(sends == 0 && closes == 0 && ctx.heartbeat_sweep_pending);
    schedule_sweep(); // Repeated ticks coalesce until the HTTP task can run.
    assert(sends == 0);
    run_work();
    assert(sends == 1 && !ctx.heartbeat_sweep_pending);
    now_s += HEARTBEAT_INTERVAL_S;
    schedule_sweep();
    run_work();
    assert(closes == 1 && !chat_sessions_is_joined(&ctx, 10));

    reset();
    join(10);
    chat_sessions_start_heartbeat(&ctx);
    sends = 0;
    now_s = HEARTBEAT_INTERVAL_S;
    schedule_sweep();
    // A close and fd reuse happen before the queued sweep, not after a stale snapshot.
    current_task = ctx.httpd_task_handle;
    assert(chat_sessions_remove_by_fd(&ctx, 10));
    assert(chat_sessions_ensure_slot(&ctx, 10));
    run_work();
    assert(sends == 0 && closes == 0);

    reset();
    chat_sessions_start_heartbeat(&ctx);
    queue_result = ESP_FAIL;
    schedule_sweep();
    assert(!ctx.heartbeat_sweep_pending && !queued_work);
    assert(!chat_sessions_broadcast_online_users(&ctx));
    assert(broadcasts == 0 && !ctx.presence_broadcast_pending);
    queue_result = ESP_OK;
    schedule_sweep();
    run_work();
    reset();

    join(10);
    chat_sessions_start_heartbeat(&ctx);
    sends = 0;
    now_s = HEARTBEAT_INTERVAL_S;
    schedule_sweep();
    run_work();
    assert(sends == 1);
    now_s += HEARTBEAT_INTERVAL_S * 3;
    schedule_sweep();
    run_work();
    assert(closes == 0 && sends == 2 && chat_sessions_is_joined(&ctx, 10));
    assert(chat_sessions_mark_alive(&ctx, 10));
    now_s++;
    schedule_sweep();
    run_work();
    assert(closes == 0);

    reset();
    chat_sessions_start_heartbeat(&ctx);
    assert(chat_sessions_ensure_slot(&ctx, 10));
    now_s = WS_JOIN_TIMEOUT_S * 3;
    schedule_sweep();
    run_work();
    assert(closes == 0);
    for (int i = 0; i < WS_JOIN_TIMEOUT_S; i++) {
        now_s++;
        schedule_sweep();
        run_work();
    }
    assert(closes == 1);
    reset();
    puts("Heartbeat regressions passed: HTTP task ownership, fd reuse and queue failures");
}
