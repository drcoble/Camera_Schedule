# FR-7 — Schedule anchors

The **schedule anchor** is the user-facing primitive for binding camera
action rules to computed solar / lunar / seasonal / calendar events. Each
anchor surfaces in the camera's Action Rules UI as its own ACAP event
topic; operators wire it to whatever action they want (recording, MQTT,
day/night switch, etc.) using the camera's existing rule engine.

## FR-7.1 — Anchor data model

An anchor SHALL be a record with the following fields:

| Field | Required | Description |
|---|---|---|
| `id` | yes | Stable, unique identifier (lowercase letters, digits, underscore). Used as the topic-segment in the registered event. |
| `name` | yes | Human label (e.g. "30 min before sunrise"). Surfaced as the event's *NiceName* in the Action Rules UI. |
| `event_source` | yes | One of: any [FR-3](./03-solar-events.md) solar event, any [FR-4](./04-lunar-events.md) lunar event, any [FR-5](./05-seasonal-events.md) seasonal event, or any [FR-6](./06-user-calendar-dates.md) calendar entry id. |
| `offset_minutes` | no, default 0 | Signed integer minutes applied to the source event time. Negative = before, positive = after. Range −1440…+1440. |
| `duration_minutes` | no | If set, the anchor produces a **stateful** event topic that goes high at `event_time + offset` and low at `start + duration`. If unset, the anchor produces a **pulse** event topic. |
| `enabled` | yes | Boolean. Disabled anchors are still registered as topics but never fire. |

## FR-7.2 — Built-in anchors

The app SHALL register one anchor per built-in event listed in
[27-reuse-from-timelapse2.md](./27-reuse-from-timelapse2.md) at boot,
with `offset_minutes=0`, `duration_minutes=null`, `enabled=true`. These
are non-deletable but can be disabled. They give operators a working
"sunrise / sunset / civil dawn / …" set without configuring anything.

## FR-7.3 — Operator-defined anchors

Operators SHALL be able to create, edit, enable/disable, and delete
custom anchors via the configuration UI ([FR-11](./11-configuration-ui.md)).
A soft cap of **64 operator-defined anchors** applies (UI rendering and
ACAP event-topic count are the limits, not a hard technical ceiling).

## FR-7.4 — Paired anchors (interval form)

For interval semantics that span two distinct events (e.g. "civil dawn
→ civil dusk" → daylight interval), the operator MAY define a paired
anchor that names two `event_source` references — one for `start`, one
for `end` — each with its own optional offset. The resulting topic
SHALL be **stateful**: high while inside the interval, low outside.
Pairs that cross local midnight are valid (e.g. "sunset → sunrise next
day" → night interval).

## FR-7.5 — Anchor reuse

The same `event_source` MAY be referenced by multiple anchors with
different offsets, durations, names, and ids. Anchors are independent
event topics; operators bind whichever they need.

## FR-7.6 — Topic naming

Each anchor's registered topic SHALL be of the form

```
tnsaxis:CameraApplicationPlatform/<appName>/<anchor.id>
```

This is the convention the camera's Action Rules UI displays as
"`<appName> > <anchor.name>`". `<appName>` comes from the manifest.

## FR-7.7 — Numeric-threshold anchors

Anchors that reference numeric values (e.g. moon illumination ≥ 0.95)
SHALL be supported. The recompute job
([FR-10](./10-recompute-cadence.md)) evaluates the threshold per local
civil day for the configured look-ahead window and arms a per-day timer
for any day on which the threshold is satisfied.

## Notes

- The anchor data model is preserved verbatim from the pre-rewrite draft
  ([DL-05](./28-decision-log.md)); only the *expansion* changed (event
  topics replaced iCalendar VEVENTs).
- Stateful (interval) topics consume one ACAP event topic each, the
  same as pulse topics. There is no separate "schedule" resource.
