# FR-8 — ACAP event topic registration

> **History.** This file replaces the original "iCalendar generation"
> requirement set. The Event Schedule REST API that approach targeted
> does not exist on shipping AXIS OS firmware (see
> [DL-05](./28-decision-log.md) and
> [26-discovered-environment.md](./26-discovered-environment.md)). The
> app now publishes ACAP event topics instead.

At boot, the app registers one ACAP event topic per anchor with the
camera's event engine. Operators see the registered topics in the
Action Rules UI under the app's name, exactly as they see Timelapse2's
`Sun noon` topic on the lab OS 12 camera today.

## FR-8.1 — Declarative event source

The set of events to register SHALL be derived from two inputs:

1. The static **built-ins file** at `app/settings/events.json`, listing
   every solar / lunar / seasonal event the app supports
   ([list in 27-reuse-from-timelapse2.md](./27-reuse-from-timelapse2.md)).
2. The **persisted operator config** loaded from `localdata/`
   ([FR-12](./12-configuration-persistence.md)), containing
   operator-defined anchors ([FR-7](./07-schedule-anchors.md)) and
   calendar entries ([FR-6](./06-user-calendar-dates.md)).

## FR-8.2 — Topic shape

Each registered topic SHALL be of the form

```
tnsaxis:CameraApplicationPlatform/<appName>/<anchor.id>
```

with `<appName>` equal to the manifest `appName`. The topic SHALL carry
a `NiceName` equal to the anchor's `name` field, so the camera's UI
displays a human label.

## FR-8.3 — Pulse vs. stateful

- Anchors with no `duration_minutes` SHALL be registered as **pulse**
  events (no `state` payload; trigger semantics).
- Anchors with `duration_minutes`, and **paired anchors**
  ([FR-7.4](./07-schedule-anchors.md)), SHALL be registered as
  **stateful** events with a boolean `state` payload — high while
  inside the interval, low outside.

## FR-8.4 — Registration mechanism

Registration SHALL go through the vendored ACAP framework
(`ACAP_EVENTS_Add_Event`, [27](./27-reuse-from-timelapse2.md)), which
internally calls `ax_event_handler_declare(...)` from `axsdk/axevent.h`.
No event topics SHALL be declared in the manifest's `acapPackageConf`
section — registration is dynamic, matching the Timelapse2 pattern.

## FR-8.5 — Reconciliation on config change

When the operator adds, removes, or renames an anchor (or calendar
entry):

- **Added** anchors SHALL be declared with `ax_event_handler_declare(...)`
  and any pending timer rearmed.
- **Removed** anchors SHALL be undeclared with
  `ax_event_handler_undeclare(...)` and any pending timer cancelled.
- **Renamed** anchors (id changed) SHALL be undeclared then re-declared.
  An id-stable rename (only `name` field changed) requires only a topic
  re-declare with the new NiceName.

The reconciliation MUST NOT touch event topics declared by other apps
on the camera (the framework only enumerates this app's own topics by
construction).

## FR-8.6 — Idempotency on restart

Re-running the registration phase against an already-running event
engine (which happens on every app start, including after a crash or
upgrade) SHALL be a no-op when the desired set matches the current set.
Implementation note: the AXEvent API treats re-declarations of the
same topic key as idempotent.

## FR-8.7 — No "app prefix" on schedule names

The pre-rewrite design carried a `[ssc] ` (or similar) name prefix for
schedules so the app could find its own resources during reconciliation.
Under Path A, ACAP event topics are scoped by `<appName>` in the topic
path itself — no string-prefix discipline is needed. Removed; see
[DL-05](./28-decision-log.md).

## FR-8.8 — Enable-state gating on the firing path

The recompute pipeline ([FR-9.2](./09-event-firing.md)) SHALL consult
the enable-state store ([FR-11.7](./11-configuration-ui.md)) before
arming a GLib timer for any anchor. Anchors whose effective `enabled`
state resolves to `false` SHALL be skipped during arm; their per-event
timer source SHALL remain `NULL` until the next recompute. The
**registration path** (`ACAP_EVENTS_Add_Event` /
`ax_event_handler_declare`) SHALL be unaffected — the topic remains
declared and visible in the Action Rules UI regardless of enable
state. See [DL-18](./28-decision-log.md) for rationale.
