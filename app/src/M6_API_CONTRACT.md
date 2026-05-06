# M6 API contract — anchors, calendar, schedule preview, schedule toggles

This document is the single source of truth for the M6 milestone's
public surface. It is consumed by:

- the UI agent — to render forms, list views, and call the FastCGI
  endpoints,
- the test agent — to drive integration tests against the same
  endpoints and to exercise the on-disk schemas directly.

The C-level data model is in `app/src/anchors.h` and `app/src/calendar.h`;
this doc shows only the parts the UI needs to render forms and the
parts the test agent needs to round-trip JSON.

The implementation (Phase 2) wires the endpoints, writes the .c files,
adds `acap-build -a` entries for the new `localdata/*.json` files in
`app/Dockerfile`, and updates `settings/events.json` consumers if
needed. None of that is in scope for this doc.

---

## 0. Conventions

- **Base path**: every M6 endpoint lives under
  `/local/camera_schedule/<endpoint>` — same prefix as the existing
  M2 `/about` and `/location` endpoints.
- **Auth**: enforced via the manifest `httpConfig.access` field per
  endpoint (DL-13). Admin-write endpoints reject HTTP `POST` with
  HTTP 403 when the session role is `viewer` or `operator`.
- **Content-Type**: JSON bodies, `application/json`; non-JSON request
  bodies on POST endpoints return HTTP 415.
- **Times**: every UTC time on the wire is an ISO-8601 string with
  trailing `Z` (e.g. `"2026-05-31T22:14:07Z"`). Local-civil-time fields
  use the same format with the camera's UTC offset (e.g.
  `"2026-05-31T18:14:07-04:00"`).
- **Dates** (calendar entries): plain `"YYYY-MM-DD"` strings.
- **IDs**: lowercase letters, digits, underscore. Regex
  `^[a-z0-9_]{1,32}$`. Validated server-side; HTTP 400 on mismatch.
- **ID namespace**: a single global namespace covers built-in anchor
  IDs (the 22 from `app/settings/events.json`), operator anchor IDs,
  and calendar-entry IDs. A POST that would create a duplicate
  returns HTTP **409 Conflict**.
- **Error envelope**: failures return `{"error":"<machine-tag>",
  "message":"<human-readable>"}` plus the appropriate HTTP status.

---

## 1. Endpoint reference

### 1.1 `GET /local/camera_schedule/anchors`

**Auth**: viewer-readable.

Returns the full anchor list — built-ins followed by operator-defined
anchors in declaration order.

```json
{
  "built_in": [
    {
      "id": "sunrise",
      "name": "Sunrise",
      "kind": "offset",
      "built_in": true,
      "enabled": true,
      "event_source": "sunrise",
      "offset_minutes": 0,
      "duration_minutes": 0
    }
    // ...22 total built-ins
  ],
  "operator": [
    {
      "id": "before_sunrise_30",
      "name": "30 min before sunrise",
      "kind": "offset",
      "built_in": false,
      "enabled": true,
      "event_source": "sunrise",
      "offset_minutes": -30,
      "duration_minutes": 0
    }
    // ...
  ]
}
```

### 1.2 `POST /local/camera_schedule/anchors`

**Auth**: admin-write (DL-13).

Two modes, distinguished by request body shape:

**Single-anchor upsert** — one operator anchor at a time:

```json
{
  "mode": "upsert",
  "anchor": { /* anchor object, kind-specific shape (§2.2) */ }
}
```

If `anchor.id` matches an existing operator anchor, it is replaced
(FR-8.5 reconciliation). If it matches a built-in, HTTP **403** with
`error: "builtin_immutable"`. Built-in `enabled` toggles go through
the `events` endpoint (§1.7), not here.

**Replace-all** — atomic bulk swap of the operator-anchor list:

```json
{
  "mode": "replace_all",
  "anchors": [ /* array of anchor objects */ ]
}
```

The built-in 22 are untouched. Validation is all-or-nothing: any
element failing per-field validation rejects the whole batch with a
`validation_error` envelope including the offending index.

**Status codes**:

| Code | Meaning |
|---|---|
| 200 | success; response body is the new full anchor list (same shape as §1.1) |
| 400 | malformed JSON, invalid field, or out-of-range value |
| 403 | viewer/operator session, or attempt to mutate a built-in |
| 404 | upsert mode targeted an id that is not a known anchor (and you didn't intend to create it) — see "create vs. replace" note below |
| 409 | id collides with another anchor or with a calendar entry |
| 413 | replace-all batch exceeds `ANCHORS_OPERATOR_MAX` (64) |
| 422 | referenced `event_source` not found in the global namespace |
| 500 | persistence or AXEvent declare/undeclare failure (server rolled back) |

> **Create vs. replace.** The single-anchor upsert mode unconditionally
> creates the anchor when its id is new, and replaces when it exists.
> No separate "create" verb. UI implementors should preflight with
> `GET /anchors` to know which they're doing if they want to label the
> button differently.

### 1.3 `DELETE /local/camera_schedule/anchors?id=<anchor_id>`

**Auth**: admin-write.

Deletes one operator anchor. Built-ins return HTTP 403
(`error: "builtin_immutable"`). Returns the new full anchor list on
success.

> **Dangling references are not blocked.** If another operator anchor
> references the to-be-deleted anchor as its `event_source` (or paired
> start/end), the delete still succeeds. The dependent anchor stays
> declared but will be silently skipped on arm at recompute time and a
> WARN line will appear in syslog. Active referential-integrity
> validation is the UI's job (FR-11.5) — show the operator a "this
> anchor is referenced by N others" prompt before they confirm.

### 1.4 `GET /local/camera_schedule/calendar`

**Auth**: viewer-readable.

```json
{
  "entries": [
    {
      "id": "thanksgiving",
      "name": "Thanksgiving",
      "kind": "annual",
      "time_mode": "all_day",
      "start_date": "2026-11-26",
      "enabled": true,
      "notes": ""
    }
    // ...
  ]
}
```

Field shape varies by `kind` and `time_mode` per §2.3.

### 1.5 `POST /local/camera_schedule/calendar`

**Auth**: admin-write.

Same two-mode shape as `POST /anchors`:

```json
{ "mode": "upsert",      "entry":   { /* calendar object */ } }
{ "mode": "replace_all", "entries": [ /* array */ ] }
```

Status codes mirror §1.2 with these specifics:

- 400 on malformed dates (invalid month/day, end_date < start_date,
  Feb 29 used as an annual entry [reject — operator should use
  Feb 28 or Mar 1, see "Feb 29 handling" below]),
- 409 on id collision across the global namespace,
- 413 on > `CALENDAR_OPERATOR_MAX` (64).

> **Feb 29 handling.** Annual entries on Feb 29 are rejected at
> validation time with HTTP 400, `error: "invalid_annual_date"`. We
> won't silently shift the recurrence — the operator picks Feb 28 or
> Mar 1 deliberately.

### 1.6 `DELETE /local/camera_schedule/calendar?id=<entry_id>`

**Auth**: admin-write.

Deletes one calendar entry. Same dangling-reference behavior as
§1.3 — anchors that reference the deleted entry are not auto-cleaned.

### 1.7 `POST /local/camera_schedule/events`

**Auth**: admin-write (DL-13). Per FR-11.6 the Schedule list view's
per-row toggle hits this endpoint with no page-level Save action.

Toggles the FR-11.7 enable state for one schedule (built-in anchor,
operator anchor, or calendar entry). Persists through
`localdata/schedule_enabled.json` (§3.3). Does NOT touch the AXEvent
declaration table per DL-18 — only the firing path is gated.

```json
{ "id": "sunrise", "enabled": false }
```

**Status codes**:

| Code | Meaning |
|---|---|
| 200 | success; response body `{"id":"...","enabled":<bool>}` |
| 400 | missing `id`, non-boolean `enabled`, or unknown id |
| 403 | viewer/operator session |
| 500 | persistence failure (rolled back) |

### 1.8 `GET /local/camera_schedule/events_today`

**Auth**: viewer-readable.

Returns the per-row data needed to render the FR-11.6 Schedule list
view. Each row carries the next-fire time within the current
look-ahead window (NFR-2 default: 90 days starting at local civil
midnight of "today"); rows with no fire in the window report
`not_firing_today` with a machine-readable reason code.

Optional query parameter:

- `?lookahead_days=<n>` — clamp [1, 365], default 1. The Schedule list
  view passes `lookahead_days=1` for "today only"; the per-anchor
  preview pane (FR-11.2 Capabilities, FR-7.4 future test) can pass
  larger values.

**Response**:

```json
{
  "computed_at_utc": "2026-05-06T12:00:00Z",
  "lookahead_days": 1,
  "lat": 33.749,
  "lon": -84.388,
  "rows": [
    {
      "id": "sunrise",
      "name": "Sunrise",
      "category": "solar",
      "kind": "offset",
      "topic": "tnsaxis:CameraApplicationPlatform/camera_schedule/sunrise",
      "enabled": true,
      "stateful": false,
      "next_fire_utc":  "2026-05-06T10:42:13Z",
      "next_fire_local": "2026-05-06T06:42:13-04:00"
    },
    {
      "id": "moonrise",
      "name": "Moonrise",
      "category": "lunar",
      "kind": "offset",
      "topic": "tnsaxis:CameraApplicationPlatform/camera_schedule/moonrise",
      "enabled": true,
      "stateful": false,
      "next_fire_utc": null,
      "next_fire_local": null,
      "not_firing_today": true,
      "not_firing_reason": "lunar_no_event"
    },
    {
      "id": "fullmoon",
      "name": "Full moon",
      "category": "lunar",
      "kind": "offset",
      "topic": "tnsaxis:CameraApplicationPlatform/camera_schedule/fullmoon",
      "enabled": true,
      "stateful": false,
      "next_fire_utc":  "2026-05-31T22:14:07Z",
      "next_fire_local": "2026-05-31T18:14:07-04:00"
    },
    {
      "id": "junesolstice",
      "name": "Longest Day",
      "category": "seasonal",
      "kind": "offset",
      "topic": "tnsaxis:CameraApplicationPlatform/camera_schedule/junesolstice",
      "enabled": true,
      "stateful": false,
      "next_fire_utc":  "2026-06-21T08:24:00Z",
      "next_fire_local": "2026-06-21T04:24:00-04:00"
    },
    {
      "id": "before_sunrise_30",
      "name": "30 min before sunrise",
      "category": "anchor",
      "kind": "offset",
      "topic": "tnsaxis:CameraApplicationPlatform/camera_schedule/before_sunrise_30",
      "enabled": true,
      "stateful": false,
      "next_fire_utc":  "2026-05-06T10:12:13Z",
      "next_fire_local": "2026-05-06T06:12:13-04:00"
    },
    {
      "id": "daylight_interval",
      "name": "Daylight",
      "category": "anchor",
      "kind": "paired",
      "topic": "tnsaxis:CameraApplicationPlatform/camera_schedule/daylight_interval",
      "enabled": true,
      "stateful": true,
      "next_fire_utc":   "2026-05-06T10:42:13Z",
      "next_fire_local": "2026-05-06T06:42:13-04:00",
      "next_end_utc":    "2026-05-07T00:21:49Z",
      "next_end_local":  "2026-05-06T20:21:49-04:00"
    },
    {
      "id": "thanksgiving",
      "name": "Thanksgiving",
      "category": "calendar",
      "kind": "annual",
      "topic": "tnsaxis:CameraApplicationPlatform/camera_schedule/thanksgiving",
      "enabled": false,
      "stateful": true,
      "next_fire_utc":  null,
      "next_fire_local": null,
      "not_firing_today": true,
      "not_firing_reason": "disabled"
    }
  ]
}
```

**Per-row field reference**:

| Field | Always present | Notes |
|---|---|---|
| `id` | yes | The schedule's id. |
| `name` | yes | Human label (rendered NiceName for built-ins post-`apply_seasonal_labels`). |
| `category` | yes | One of `solar`, `lunar`, `seasonal`, `anchor`, `calendar`. Drives FR-11.6 grouping. |
| `kind` | yes | `offset` / `paired` / `threshold` for anchors; `single_date` / `date_range` / `annual` for calendar; `offset` for built-ins. |
| `topic` | yes | Full AXEvent topic path. UI may copy/paste into MQTT documentation. |
| `enabled` | yes | Snapshot of the FR-11.7 store. UI uses it to position the toggle. |
| `stateful` | yes | True iff the topic is registered as stateful (paired anchor, offset anchor with duration_minutes > 0, calendar entry with `time_mode: all_day`). |
| `next_fire_utc` / `next_fire_local` | yes | UTC and local-civil-time ISO-8601 of the next fire within the look-ahead window. Both are `null` when `not_firing_today` is true. |
| `next_end_utc` / `next_end_local` | only when `stateful: true` | Time the topic transitions back to low. Same null rule. |
| `not_firing_today` | optional, default false | True when no fire is in the look-ahead window or the schedule is disabled. |
| `not_firing_reason` | only when `not_firing_today: true` | One of: `disabled`, `polar_no_event`, `lunar_no_event`, `solar_no_event`, `out_of_range`, `dependency_missing`, `dependency_cycle`, `threshold_unmet`. |

---

## 2. Anchor model — UI form summary

The UI agent should render three different forms keyed off `kind`. The
test agent should produce one fixture per kind. This section
summarizes the wire shape; for the full C struct see `anchors.h`.

### 2.1 Common fields (all kinds)

```json
{
  "id":       "<^[a-z0-9_]{1,32}$>",
  "name":     "<1..64 chars>",
  "kind":     "offset" | "paired" | "threshold",
  "enabled":  true,
  "built_in": false       // ignored on POST; always false for operator
}
```

### 2.2 Per-kind fields

**`kind: "offset"`** — single source ± offset. Pulse if
`duration_minutes` is 0; stateful otherwise.

```json
{
  "kind": "offset",
  "event_source":     "<id of any built-in anchor / operator anchor / calendar entry>",
  "offset_minutes":   -30,    // [-1440, 1440]
  "duration_minutes":  0      // 0 = pulse; (0, 1440] = stateful
}
```

**`kind: "paired"`** — interval bounded by two source events. Always
stateful (FR-7.4).

```json
{
  "kind": "paired",
  "start_event":          "civildawn",
  "start_offset_minutes":  0,
  "end_event":            "civildusk",
  "end_offset_minutes":    0
}
```

When `end_event`'s resolved local-day instant precedes `start_event`'s,
the next-day occurrence of `end_event` is used (so "sunset → sunrise"
spans local midnight correctly).

**`kind: "threshold"`** — fires once per local civil day in the
look-ahead window when a numeric metric satisfies the comparison.

```json
{
  "kind":   "threshold",
  "metric": "moon_illumination",   // v1: only this value is allowed
  "op":     "ge",                  // ge | le | gt | lt
  "value":  0.95
}
```

The metric is sampled at **local solar noon** of each day in the look-
ahead window. The fire instant on a satisfying day is **local solar
midnight of that day** — see open question OQ-13 below; this default
is chosen for predictability and to align with the existing
`recompute_today` cadence in `timers.c`. Phase 2 may revise.

### 2.3 Calendar wire shape

```json
{
  "id":         "<^[a-z0-9_]{1,32}$>",
  "name":       "<1..64 chars>",
  "kind":       "single_date" | "date_range" | "annual",
  "time_mode":  "all_day" | "specific",
  "time_of_day":"HH:MM:SS",     // required iff time_mode = "specific"
  "start_date": "YYYY-MM-DD",   // for annual: only month/day used
  "end_date":   "YYYY-MM-DD",   // required iff kind = "date_range"
  "notes":      "<0..256 chars>",
  "enabled":    true            // ignored on POST; new entries default true
}
```

---

## 3. On-disk schemas

All three files live in the app's `localdata/` directory and are
written via `ACAP_FILE_Write` (atomic write-temp + fsync + rename per
FR-12.1). The acap-build invocation does NOT need to bundle these
because `localdata/` is created at runtime by the framework — but the
test agent should clear them between fixtures.

### 3.1 `localdata/anchors.json`

Top-level array of operator-defined anchors. Built-ins are NOT
persisted here — they are derived from `app/settings/events.json` at
boot and exist only in memory.

```json
[
  {
    "id": "before_sunrise_30",
    "name": "30 min before sunrise",
    "kind": "offset",
    "enabled": true,
    "event_source": "sunrise",
    "offset_minutes": -30,
    "duration_minutes": 0
  },
  {
    "id": "daylight_interval",
    "name": "Daylight",
    "kind": "paired",
    "enabled": true,
    "start_event": "civildawn",
    "start_offset_minutes": 0,
    "end_event": "civildusk",
    "end_offset_minutes": 0
  },
  {
    "id": "bright_moon_pulse",
    "name": "Bright moon",
    "kind": "threshold",
    "enabled": true,
    "metric": "moon_illumination",
    "op": "ge",
    "value": 0.95
  }
]
```

Schema rules enforced before the atomic rename (FR-12.1 validation
hook):

- top-level value is a JSON array, length ≤ `ANCHORS_OPERATOR_MAX` (64);
- every element is an object containing all fields required for its
  `kind`; unknown fields are rejected (no silent stripping);
- IDs match `^[a-z0-9_]{1,32}$` and are unique within the array;
- IDs do not collide with any built-in anchor id or any calendar entry
  id (cross-namespace check);
- `offset_minutes` ∈ [-1440, 1440]; `duration_minutes` ∈ [0, 1440];
- `metric` is `"moon_illumination"`; `op` ∈ {ge,le,gt,lt}; `value` is
  a finite number in [0.0, 1.0] (range applies for the v1 metric).

A failed schema check at write time aborts the rename and leaves the
previous file untouched (the API call returns HTTP 500 with
`error: "persist_failed"`). On startup, a malformed file is renamed
`anchors.json.broken-<unix-ts>` and the in-memory list starts empty
(FR-12.4).

### 3.2 `localdata/calendar.json`

Top-level array of calendar entries. Same atomic-write contract.

```json
[
  {
    "id": "thanksgiving",
    "name": "Thanksgiving",
    "kind": "annual",
    "time_mode": "all_day",
    "start_date": "2026-11-26",
    "notes": "",
    "enabled": true
  },
  {
    "id": "new_years_eve_party",
    "name": "NYE security boost",
    "kind": "single_date",
    "time_mode": "specific",
    "time_of_day": "20:00:00",
    "start_date": "2026-12-31",
    "notes": "Higher-frequency recording",
    "enabled": true
  },
  {
    "id": "summer_camp_2026",
    "name": "Summer camp",
    "kind": "date_range",
    "time_mode": "all_day",
    "start_date": "2026-06-15",
    "end_date":   "2026-08-10",
    "notes": "",
    "enabled": true
  }
]
```

### 3.3 `localdata/schedule_enabled.json` (FR-11.7, DL-18)

Flat ID-keyed object. **Absent keys mean enabled.** Only entries that
have ever been explicitly toggled are stored — new built-ins
introduced by an `.eap` upgrade default to enabled until the operator
flips them.

```json
{
  "moonset": false,
  "before_sunrise_30": false
}
```

Schema rules: top-level object whose every value is a boolean; keys
are not constrained against the live anchor / calendar list (orphan
keys are tolerated per FR-11.7 — they are silently ignored at boot
when no schedule with that id is registered).

---

## 4. Reconciliation semantics

These paragraphs codify what the FastCGI handlers MUST do when
operator-driven changes hit `anchors.json` / `calendar.json`. The
implementation lives in `anchors.c` and `calendar.c` in Phase 2; the
behavior is what the test agent will assert against.

**Add (create new operator anchor or calendar entry).** Validate the
request → write to localdata atomically → call
`ACAP_EVENTS_Add_Event(id, name, state)` to declare the new AXEvent
topic → trigger a recompute that arms the new slot if its next fire
is inside the look-ahead window. The new topic is visible in the
camera's Action Rules UI immediately on success of the POST. Rollback
on any step's failure: in-memory list, on-disk file, AXEvent table all
return to pre-call state.

**Remove (delete operator anchor or calendar entry).** Cancel any
armed GLib timer for the id → call `ACAP_EVENTS_Remove_Event(id)` to
undeclare → write the new (smaller) localdata file atomically →
sweep `schedule_enabled.json` only if the operator explicitly opted
into orphan cleanup (M7 work; for M6 we tolerate the orphan key per
FR-11.7). Dangling references from other anchors are NOT cleaned up
synchronously — they surface as recompute-time WARN logs (see §1.3).

**Rename — id-stable (only `name` changed).** Per FR-8.5, this is a
re-declare with the new NiceName: `ACAP_EVENTS_Remove_Event(id)`
followed by `ACAP_EVENTS_Add_Event(id, new_name, state)`. Armed
timers are NOT cancelled — the topic id is the same and any pre-bound
action rule continues to work. Same pattern the existing
`apply_seasonal_labels` in `main.c` uses for hemisphere-aware solstice
labels (FR-5.2).

**Rename — id changed.** This is modeled as a delete followed by a
create at the API level: cancel timers under the old id, undeclare,
declare under the new id, arm fresh timers. Operators who had bound
action rules to the old id will see the rule break in the camera's UI
— the same dangling-rule outcome they'd see if they'd deleted and
recreated. The UI agent should warn before submitting an id-change.

**Paired anchor edits.** Editing either `start_event` or `end_event`
(or their offsets) is treated as a content edit, not a rename: the
topic stays declared, armed timers are cancelled, the anchor is
re-armed from the new spec on the next recompute. Editing `id`
follows the rule above. The stateful payload contract
(`ACAP_EVENTS_Fire_State(id, true/false)`) is unchanged.

**Threshold anchor edits.** Any field change (metric, op, value)
triggers a full re-evaluation of the look-ahead window because the
set of qualifying days is a function of all three. Implementation
note for Phase 2: re-arming a 90-day threshold anchor is bounded by
how cheaply `lunar_illumination(time_t)` can be sampled at 90 noons
— at ~milliseconds per sample on the lab cameras this is comfortable
within NFR-2's 2 s recompute budget.

---

## 5. Open questions surfaced for the orchestrator

**OQ-13 — When does a numeric-threshold anchor pulse fire on a
satisfying day?** FR-7.7 says "arms a per-day timer ... on satisfying
days" without pinning a time of day. Two defensible answers:

1. **Local solar midnight of the satisfying day** (chosen as the
   default in this contract). Aligns with the existing FR-10.1 daily
   recompute trigger; predictable; observer-relevant only insofar as
   the day's qualification is observer-neutral for the v1 metric
   (illumination).
2. **The astronomical maximum of the metric on that day** —
   for moon illumination, that is roughly the lunar transit closest
   to full moon. More natural ("fire when the moon is brightest") but
   requires an extra computation per qualifying day.

The contract is shipped with answer (1) so Phase 2 has something to
implement and the UI / test agents can proceed. The orchestrator
should resolve before integration if (2) is preferred — the change
would be confined to `anchors.c`'s threshold-arm helper and one fixture
in the test agent's `test_anchors.c`.

**OQ-14 — `events_today` `not_firing_reason` taxonomy stability.**
The reason codes listed in §1.8 are small but if the UI agent surfaces
them as i18n strings they become a public surface that's painful to
extend. Phase 2 should either (a) emit only the codes documented here
and reject contributions of new codes without a doc update, or (b)
add a free-form `not_firing_message` field alongside the code. The
contract picks (a) implicitly by enumerating the codes; flag if the
UI agent finds it constraining.
