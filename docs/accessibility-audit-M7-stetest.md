# Accessibility audit — M7 STE independent verification

This document is the STE's independent re-run of the FR-11.4 accessibility
checklist. It is produced in parallel with (not derived from)
`docs/accessibility-audit-M7.md` (UI agent). Discrepancies between this
document and the UI agent's audit are flagged with:

> **[DISCREPANCY: UI says X, STE found Y]**

The integrator resolves discrepancies at merge time before the M7
acceptance gate is closed.

---

## Audit scope

Pages audited: `about.html`, `location.html`, `schedule.html`,
`anchors.html`, `calendar.html`.

All new M7 UI surface from `M7_API_CONTRACT.md §5`:
- Status panel (§5.1)
- Toolbar buttons: Recompute now, Export, Import modal (§5.2)
- Debug-logging toggle (§5.3)

Checklist items (per FR-11.4 and M7_API_CONTRACT.md §5.4):

1. Every interactive element keyboard-reachable.
2. Visible focus styles (not just `:hover`).
3. Labeled form fields (`<label for=...>` or `aria-label`).
4. ARIA roles on dynamic regions (`role="list"`, `aria-live`).
5. Modal traps focus while open; restores focus on close.
6. Color contrast meets WCAG 2.1 AA (4.5:1 body / 3:1 large text).
7. Error messages announced to screen readers (`aria-live="polite"`).

---

## Status

**Pending UI agent deliverable.**

This document is the structured checklist pending population against the
UI agent's committed output in `docs/accessibility-audit-M7.md` and the
actual HTML/CSS/JS changes in `app/html/`. Once those are committed, the
STE will:

1. Manually tab through all 5 pages in a browser with a keyboard only
   (no mouse) and mark each item below.
2. Run WAVE or axe-core against each page (browser extension or
   `npx axe-cli`) for automated color-contrast and ARIA checks.
3. Compare results against the UI agent's audit and note any
   discrepancies.

---

## Checklist — per page

### about.html

| # | Item | Result | Notes |
|---|------|--------|-------|
| 1 | All interactive elements keyboard-reachable | — | pending |
| 2 | Visible focus styles | — | pending |
| 3 | Labeled form fields | — | no form fields expected |
| 4 | ARIA roles on dynamic regions | — | pending |
| 5 | Modal focus trap | — | no modal expected |
| 6 | Color contrast WCAG 2.1 AA | — | pending |
| 7 | Error messages aria-live | — | pending |

### location.html

| # | Item | Result | Notes |
|---|------|--------|-------|
| 1 | All interactive elements keyboard-reachable | — | lat/lon inputs + override button |
| 2 | Visible focus styles | — | pending |
| 3 | Labeled form fields | — | lat/lon fields need `<label for>` or `aria-label` |
| 4 | ARIA roles on dynamic regions | — | pending |
| 5 | Modal focus trap | — | n/a |
| 6 | Color contrast WCAG 2.1 AA | — | pending |
| 7 | Error messages aria-live | — | invalid lat/lon error display |

### schedule.html (M7 additions: status panel, toolbar, debug toggle)

| # | Item | Result | Notes |
|---|------|--------|-------|
| 1 | All interactive elements keyboard-reachable | — | status panel collapse, Recompute, Export, Import, Debug toggle |
| 2 | Visible focus styles | — | pending |
| 3 | Labeled form fields | — | Import file picker; Debug toggle checkbox |
| 4 | ARIA roles on dynamic regions | — | status panel `aria-live`; import modal `role="dialog"` |
| 5 | Modal focus trap (Import modal) | — | focus must stay inside modal while open |
| 6 | Color contrast WCAG 2.1 AA | — | pending |
| 7 | Error messages aria-live | — | import error envelope, recompute failure toast |

#### M7-specific items for schedule.html

| Item | Expected behavior | Result |
|------|-------------------|--------|
| Status panel is collapsible | Toggle button keyboard-activatable; `aria-expanded` state correct | — |
| "Recompute now" button | Spinner state communicated to screen readers (`aria-busy` or live region) | — |
| Export button/link | Keyboard-activatable; announces download intent | — |
| Import file picker | `<input type="file">` has accessible label | — |
| Import modal | `role="dialog"`, `aria-labelledby`, focus trapped on open, returned on close | — |
| Import error display | Error text in `aria-live="polite"` region | — |
| Debug-logging toggle | Checkbox with `<label>` or `aria-label`; state change announced | — |
| Recent activity expand/collapse | `aria-expanded` attribute present | — |

### anchors.html

| # | Item | Result | Notes |
|---|------|--------|-------|
| 1 | All interactive elements keyboard-reachable | — | create/edit/delete inline actions |
| 2 | Visible focus styles | — | pending |
| 3 | Labeled form fields | — | anchor name, event source, offset fields |
| 4 | ARIA roles on dynamic regions | — | anchor list `role="list"` |
| 5 | Modal focus trap | — | edit modal (if any) |
| 6 | Color contrast WCAG 2.1 AA | — | pending |
| 7 | Error messages aria-live | — | pending |

### calendar.html

| # | Item | Result | Notes |
|---|------|--------|-------|
| 1 | All interactive elements keyboard-reachable | — | date inputs, kind selector |
| 2 | Visible focus styles | — | pending |
| 3 | Labeled form fields | — | date fields, kind dropdowns |
| 4 | ARIA roles on dynamic regions | — | pending |
| 5 | Modal focus trap | — | pending |
| 6 | Color contrast WCAG 2.1 AA | — | pending |
| 7 | Error messages aria-live | — | pending |

---

## Known pre-M7 baseline issues

The following are pre-existing issues from M6 that the STE expects to see
fixed in the M7 a11y pass (UI agent owns the fixes):

- `onclick` handlers on non-interactive elements (e.g. schedule list row
  toggles) need `tabindex="0"` and Enter/Space key handler companions.
- Enable/disable row toggles in the schedule list: verify these are
  `<input type="checkbox">` or `<button>` — not bare `<div>` click targets.
- "Saving…" and "Failed — retry?" transient states: confirm they are in
  an `aria-live` region so screen readers announce them.

These items will be reconciled against the UI agent's audit once that
document is committed.

---

## Tools used

- Manual keyboard navigation (Tab, Shift+Tab, Enter, Space, Escape).
- Browser WAVE extension for automated contrast and ARIA checks.
- `axe-core` via browser devtools for programmatic ARIA validation.

---

## Next steps

1. Wait for UI agent to commit `docs/accessibility-audit-M7.md` and
   the HTML/CSS/JS changes.
2. Populate the result columns above from independent manual testing.
3. Flag discrepancies with `[DISCREPANCY: ...]` markers.
4. Integrator reconciles before M7 acceptance gate is closed.
