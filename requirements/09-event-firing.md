# FR-9 — Event firing

> **History.** This file replaces the original "Writing schedules to the
> camera (VAPIX)" requirement. The Event Schedule REST API that approach
> targeted does not exist on shipping AXIS OS firmware (see
> [DL-05](./28-decision-log.md) and
> [26-discovered-environment.md](./26-discovered-environment.md)). The
> app now fires the topics it registered in
> [FR-8](./08-event-registration.md) at the computed local times.

Once event topics are registered (FR-8) and the per-day computed event
times are known (FR-3 / FR-4 / FR-5 / FR-6 / FR-7), the app's job is to
fire those topics at the right local times. This is done with GLib
timer sources on the ACAP main loop — the model Timelapse2 already
uses.

## FR-9.1 — Midnight scheduler

The app SHALL maintain one GLib timer source that fires once per local
civil day, at **solar midnight** when computable (FR-3.3) or at
**02:30 local time** as a fallback (matches FR-10.1). On firing, the
midnight callback SHALL:

1. Recompute the full event-time set for the new local civil day
   ([FR-10](./10-recompute-cadence.md)).
2. Cancel any per-event timers carried over from the previous day.
3. Arm a new per-event timer for each event whose local time falls
   within the look-ahead window starting today.
4. Reschedule itself for the next civil day.

## FR-9.2 — Per-event one-shot timers

For each anchor that has a computed event time today (or within the
arming horizon), the app SHALL allocate a one-shot
`g_timeout_source_new_seconds(seconds_until_event)` whose callback:

- For pulse anchors: calls `ACAP_EVENTS_Fire(anchor.id)` (which wraps
  `ax_event_handler_send_event` with a synthetic `int "value"` payload),
  then returns `G_SOURCE_REMOVE`.
- For stateful (interval) anchors: at start time, calls
  `ACAP_EVENTS_Fire_State(anchor.id, true)`; arms a second one-shot for
  end time which calls `ACAP_EVENTS_Fire_State(anchor.id, false)`.
- For numeric-threshold anchors ([FR-7.7](./07-schedule-anchors.md)):
  same as pulse, but only armed on days the threshold is satisfied.

Returning `G_SOURCE_REMOVE` ensures the source is freed by GLib; the
app SHALL keep no separate global handle table beyond what's needed
for cancellation on config or location change.

## FR-9.3 — Cancellation on change

When the operator edits an anchor / calendar entry, or when the
camera's lat/lon or timezone changes, the app SHALL:

1. Cancel all currently armed per-event timers (call
   `g_source_destroy` on each).
2. Recompute the event-time set for today.
3. Arm fresh per-event timers per FR-9.2.
4. Leave the midnight scheduler (FR-9.1) untouched unless the timezone
   changed (in which case its next-fire time is recomputed).

## FR-9.4 — Drift tolerance

GLib timer sources are wall-clock based on the camera's system clock.
The app SHALL accept the precision GLib provides — typically within
1 second of the target time. No sub-second precision is required by
any FR. NTP convergence is a precondition (FR-2.6).

## FR-9.5 — Past events on startup

If the app starts mid-day (e.g. after a reboot), it SHALL skip arming
timers for events whose local time already passed. The midnight
scheduler still arms tomorrow's events at solar midnight tonight.

## FR-9.6 — No HTTP, no D-Bus on the firing path

The firing path runs entirely on-device, on the ACAP main loop, via
the AXEvent C API. **No VAPIX call**, **no D-Bus call**, **no HTTPS
loopback** is involved at firing time. The only D-Bus call the app
makes is the one-time `VAPIXServiceAccounts1.GetCredentials` used by
the geolocation read/write path ([FR-1](./01-geo-location.md)) and any
other VAPIX ops the app wants to perform during configuration.

## FR-9.7 — Failure handling

If `ax_event_handler_send_event` returns failure for a given fire:

- log at WARN with the anchor id and the underlying error
  ([FR-13](./13-logging.md)),
- do **not** retry within the same fire callback,
- continue with subsequent timers.

A persistent pattern of failures (≥ 3 in a row, any anchor) SHALL be
surfaced in the configuration UI status panel.

## FR-9.8 — No reconciliation loop, no retry budget

The pre-rewrite design carried a 30 s → 30 min exponential backoff for
write failures against the REST API and a list-diff-apply reconciliation
loop. Neither applies under Path A: there is no remote resource to
reconcile, and the firing path is in-process. Removed; see
[DL-05](./28-decision-log.md).
