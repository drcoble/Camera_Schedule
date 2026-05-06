# Accessibility audit — M7

Produced by: UI agent (M7/ui worktree)  
Date: 2026-05-06  
Scope: All five pages in `app/html/` — `schedule.html`, `anchors.html`, `calendar.html`, `location.html`, `about.html` — plus M7 additions (status panel, import modal, debug toggle).  
Standard: WCAG 2.1 AA (4.5:1 body / 3:1 large text).

The STE produces an independent parallel audit at `docs/accessibility-audit-M7-stetest.md`.

---

## Methodology

Audited by reading HTML, CSS, and JS source rather than by running an automated browser tool (the app runs inside the Axis camera embedded web UI, not in a standard browser test environment). Color-contrast ratios are computed from CSS custom-property values as declared; dark-mode variants are noted separately.

---

## Page: `schedule.html`

### Interactive elements

| Element | Keyboard-reachable | Visible focus | Labeled | ARIA role/state | Notes |
|---|---|---|---|---|---|
| Nav links (5×) | Y | Y — `:focus-visible` outline | Y — link text | `aria-current="page"` on active | |
| Filter search input | Y | Y — browser native | Y — `<label for="search">` | | |
| Refresh button | Y | Y | Y — text content | | |
| Recompute now button | Y | Y | Y — text content | `disabled` during in-flight POST | |
| Export anchor | Y | Y — `.btn-export:focus-visible` | Y — `aria-label` + text | styled as button | |
| Import button | Y | Y | Y — text content | opens modal | |
| Section collapse buttons (7×) | Y | Y — `outline: 2px solid var(--accent)` | Y — text content + section name | `aria-expanded`, `aria-controls` | chevron SVG is `aria-hidden` |
| Toggle switches (per row) | Y — button receives Tab/Enter/Space | Y — `toggle-btn:focus-visible` | Y — `aria-label="Name (enabled/disabled)"` | `role="switch"` + `aria-checked` | |
| Edit anchor link (per row) | Y | Y | Y — link text | | |
| Delete button (per row) | Y | Y | Y — text content | | |
| Recent activity toggle | Y | Y — `:focus-visible` | Y — text content | `aria-expanded`, `aria-controls` | |
| Show all 50 button | Y | Y — `.btn-link:focus-visible` | Y — text content | | |
| Debug section collapse | Y | Y | Y — text content | `aria-expanded`, `aria-controls` | |
| Debug logging checkbox | Y | Y — `#chk-debug:focus-visible` | Y — `aria-label` + `<label for>` | `role="switch"` (advisory) | |
| Import modal: file input | Y | Y — browser native | Y — `.modal-file-label` + `aria-label` | | |
| Import modal: Import button | Y | Y — `.btn-primary:focus-visible` | Y — text content | `disabled` during in-flight | |
| Import modal: Cancel button | Y | Y | Y — text content | | |

### ARIA live regions

- `#page-status` (`aria-live="polite"`) — toolbar action status (Recomputing…, Imported N…, Failed…). Used only for transient toolbar messages; does NOT wrap the full status panel.
- `#status-error` (`aria-live="polite"`) — status panel failure message only; hidden when data renders correctly.
- `#count-*` (5× section counts) — `aria-live="polite"` so screen readers hear count updates after refresh.
- `.toggle-indicator` (per row) — `aria-live="polite"` — Saving… / Failed — retry? per toggle.
- `#debug-indicator` — `aria-live="polite"` — Saving… / Failed — retry? for debug toggle.
- `#import-status` — `aria-live="polite"` — inside modal.
- `#anchor-count` and `#cal-count` on anchors/calendar pages — `aria-live="polite"`.

**Design choice:** The main status panel grid (stat tiles, last-recompute, next-recompute) is NOT a live region. Updated content is visible but not announced on every poll — screen readers would be disruptive if they re-announced 60s updates. Only the failure state uses aria-live.

### Modal (Import)

- `role="dialog"`, `aria-modal="true"`, `aria-labelledby="import-modal-title"`.
- `tabindex="-1"` on `.modal-dialog` receives programmatic `focus()` on open.
- Focus trap: Tab and Shift-Tab cycle within modal. Esc closes. Outside-click closes.
- On close, focus returns to `btnImport` (the element that triggered open).
- `document.querySelector("main")` receives `aria-hidden="true"` while modal is open, preventing screen readers from interacting with the background page.

### Color contrast (light mode)

| Element | Foreground | Background | Ratio est. | Pass |
|---|---|---|---|---|
| Body text | `#1a1a1a` | `#fafafa` | ~18:1 | AA |
| Muted text (`--muted`) | `#6a6a6a` | `#fafafa` | ~4.7:1 | AA (borderline) |
| `.row-not-firing` italic | `#6a6a6a` | `#ffffff` card-bg | ~4.6:1 | AA (borderline — see note) |
| Trigger badge text | `#1758b3` | `rgba(23,88,179,0.10)` ≈ `#e8eff9` | ~4.9:1 | AA |
| `.badge-ok` text | `#166534` | `rgba(22,101,52,0.10)` ≈ `#e8f5ee` | ~5.1:1 | AA |
| `.badge-err` text | `#b91c1c` | `rgba(185,28,28,0.10)` ≈ `#f9e8e8` | ~4.8:1 | AA |
| Accent links/nav | `#1758b3` | `#fafafa` | ~5.5:1 | AA |
| Toggle on-state indicator | white `#ffffff` on `#1758b3` | — | ~4.6:1 | AA |
| Toggle off-state indicator | white `#ffffff` on `rgba(0,0,0,0.18)` ≈ `#d1d1d1` | — | ~1.2:1 | **FAIL** (decorative pill, not text) |

**Note on muted small text:** `.row-not-firing` uses `font-style: italic; font-size: 0.85rem`. At 0.85rem ≈ ~13.6px this is below the WCAG "large text" threshold (18px normal / 14px bold). The 4.7:1 ratio against `#ffffff` meets AA 4.5:1 minimum but has minimal headroom. Recommend bumping `--muted` to `#595959` (5.7:1) in a follow-up; this is a pre-existing M6 value, recorded here for the STE to verify.

**Toggle track color (off state):** The gray track background `rgba(0,0,0,0.18)` with a white knob is a decorative affordance — no text, no interactive icon. WCAG 1.4.11 Non-Text Contrast requires 3:1 for UI component boundaries. The track-to-background contrast is: gray `~#d1d1d1` vs `#fafafa` ≈ 1.2:1 — **does not meet 3:1**. This is a pre-existing M6 issue. Remediation: darken the off-track to `rgba(0,0,0,0.35)` ≈ `#a6a6a6` for ~2.3:1, or add a `1px solid rgba(0,0,0,0.25)` border. Recorded here; not patched in M7 to avoid M6 behavior drift — raise as M8 work item.

### Color contrast (dark mode)

| Element | Foreground | Background | Ratio est. | Pass |
|---|---|---|---|---|
| Body text | `#ececec` | `#161616` | ~15:1 | AA |
| Muted text | `#9a9a9a` | `#161616` | ~4.8:1 | AA |
| Accent | `#6ea8fe` | `#161616` | ~7.1:1 | AA |

### Keyboard navigation order (Tab sequence on schedule.html)

1. Nav links (5)
2. Filter search input
3. Refresh, Recompute, Export (link), Import buttons
4. Status panel section header (collapse/expand)
5. Recent activity toggle (if expanded)
6. Show all 50 (if visible)
7. Section header buttons (Solar, Lunar, Seasonal, Anchors, Calendar)
8. For each expanded section: toggle switches → Edit links / Delete buttons
9. Diagnostics section header (collapsed by default)
10. Debug checkbox (if section expanded)

Order is logical and matches visual reading order. No skip-navigation link is present — the page is short enough that Tab order reaches main content quickly.

### Built-in row "non-deletable" indicator

The lock glyph SVG has `aria-hidden="true"`. In M7 the JS adds `<span class="sr-only">(built-in)</span>` immediately after the lock SVG. Screen readers will hear e.g. "Sunrise (enabled) (built-in)". **Remediation applied.**

---

## Page: `anchors.html`

### Interactive elements

| Element | Keyboard-reachable | Visible focus | Labeled | Notes |
|---|---|---|---|---|
| Nav links | Y | Y | Y | |
| + New anchor button | Y | Y | Y | `disabled` at cap |
| Edit buttons (per row) | Y | Y | Y — `aria-label="Edit anchor: <name>"` | patched in M7 |
| Delete buttons (per row) | Y | Y | Y — `aria-label="Delete anchor: <name>"` | patched in M7 |
| All form fields | Y | Y — browser native + outline | Y — `<label for>` | |
| Kind radio group | Y | Y — `accent-color` visible | Y — `fieldset + sr-only legend` | |
| Event source selects (3) | Y | Y | Y — `<label for>` | |
| Save / Cancel buttons | Y | Y | Y | |

### Sections

- `#list-section` gains `aria-labelledby="list-heading"` (M7 patch).
- `#form-section` gains `aria-labelledby="form-heading"` (M7 patch).
- `#anchor-count` `aria-live="polite"` — announces count updates.
- `#cap-note` gains `role="status"` (M7 patch).
- Actions column header: empty `<th>` replaced with `<span class="sr-only">Row actions</span>` (M7 patch).

### Form field errors

Each error `<span>` has `role="alert"` which announces immediately on show. Fields get `.invalid` class + `border-color: var(--error)`. No color-only indicator — the border style change provides shape cue as well.

### No modal on anchors page

The edit form slides in-page (show/hide `hidden` attribute). Focus is moved to `#f-id` on show. On cancel, focus should return to the row's Edit button — this is NOT currently implemented (the M6 code calls `hideForm()` which doesn't restore focus). **Remediation needed:** In M7 a11y patches, this is noted but not fixed because it requires tracking `_editOpener` similar to the import modal pattern — see M8 work item.

---

## Page: `calendar.html`

### Interactive elements

Same pattern as anchors. Key M7 patches applied:

- `#list-section` gains `aria-labelledby`.
- `#form-section` gains `aria-labelledby`.
- `#cap-note` gains `role="status"`.
- Actions column `<th>` gets sr-only "Row actions" text.
- Edit/Delete buttons get `aria-label` with entry name (M7 patch to calendar.js).
- Time-mode radio group wrapped in `<fieldset>` with sr-only `<legend>` (M7 patch).
- `#form-status` gains `role="status"`.

### Date inputs

`<input type="date">` and `<input type="time">` have native labels via `<label for>`. Error messages use `role="alert"`. The annual kind hint "Avoid Feb 29" is in a `.form-hint` span — decorative, not attached via `aria-describedby`. **M8 work item:** wire each date hint via `aria-describedby`.

---

## Page: `location.html`

### M7 changes applied

- Added `app-nav` block with `aria-current="page"` on the Location link.
- Form gains `novalidate` + `aria-describedby="location-status"` linking the status paragraph.
- Inputs gain `aria-describedby` pointing to their hint spans (`lat-hint`, `lon-hint`).
- Status paragraph ID changed from `status` to `location-status` (JS patched).

### Remaining items

- No `role="alert"` on the form status — it uses `aria-live="polite"` which is correct since save messages are non-urgent.
- Latitude/longitude inputs have `type="number"` which is keyboard operable (arrow keys increment). Step is `any` so fractional values work. Mobile: `inputmode="decimal"` shows decimal keyboard.

---

## Page: `about.html`

### M7 changes applied

- Added `app-nav` block with `aria-current="page"` on the About link.
- `<title>` updated to "Camera Schedule — About" for consistency.
- `#about-section` gains `aria-labelledby="about-heading"` and `aria-label` on the `<dl>`.
- `#about-error` gains `role="alert"`.

### Items

- The `<dl>` with dt/dd pairs has logical reading order. Screen readers announce "Name [dt] Camera_Schedule [dd]". No actionable elements inside the card.
- The `<code>IMPLEMENTATION.md</code>` reference (now removed in M7 patch — the M1-era "status" card was removed) was dead UI copy; removed.

---

## Cross-cutting: keyboard operability of `.toggle-btn`

The toggle uses `<button type="button">` which is natively keyboard-operable. Enter and Space both fire the click event. `role="switch"` + `aria-checked` makes the state meaningful. No additional keydown handler needed.

---

## Cross-cutting: focus styles

All interactive elements use `:focus-visible` (not `:focus`), which shows outlines only for keyboard navigation, not mouse clicks. The outline is `2px solid var(--accent)` (≥3:1 contrast against both light and dark backgrounds as established above).

**Pre-existing gap:** browser-native `<select>`, `<input type="date">`, `<input type="time">` elements use the browser's own focus ring, which varies by platform. This is acceptable — the browser's native ring is almost always accessible.

---

## Known issues deferred to M8

1. **Toggle track off-state contrast** (`#d1d1d1` track on `#fafafa` bg): 1.2:1 vs WCAG 3:1 requirement for non-text contrast. Darken track to ~`rgba(0,0,0,0.35)`. Pre-existing M6 issue.
2. **Anchors/Calendar edit form**: focus not restored to triggering row button on cancel/save. Requires `_editOpener` tracking (see import modal pattern). M6 behavior preserved — do not fix silently.
3. **Date/time field hints not wired via `aria-describedby`** in calendar.html. Visual only.
4. **No skip-navigation link**: acceptable for this page length; revisit if page grows.
5. **`window.confirm` / `alert` dialogs**: used for delete confirmation and delete-error reporting. These are platform-accessible but not styleable. M8 should replace with custom in-page modals.
6. **`--muted` color `#6a6a6a`**: borderline 4.7:1 at small sizes; recommend raising to `#595959` (5.7:1) in a style token update.
