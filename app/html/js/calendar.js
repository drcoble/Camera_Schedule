// Vanilla JS — no frameworks, no CDN (DL-08).
// GET    /local/camera_schedule/calendar        → {entries}
// POST   /local/camera_schedule/calendar        body {mode:"upsert", entry:{…}}
// DELETE /local/camera_schedule/calendar?id=…   → updated list

(function () {
  "use strict";

  var CALENDAR_MAX = 64;

  // -----------------------------------------------------------------------
  // State
  // -----------------------------------------------------------------------
  var entries = [];
  var editingId = null;

  // -----------------------------------------------------------------------
  // DOM refs
  // -----------------------------------------------------------------------
  var calCount    = document.getElementById("cal-count");
  var capNote     = document.getElementById("cap-note");
  var calTbody    = document.getElementById("cal-tbody");
  var btnNew      = document.getElementById("btn-new");
  var formSection = document.getElementById("form-section");
  var formHeading = document.getElementById("form-heading");
  var calForm     = document.getElementById("cal-form");
  var formStatus  = document.getElementById("form-status");
  var btnSubmit   = document.getElementById("btn-submit");
  var btnCancel   = document.getElementById("btn-cancel");

  var fId          = document.getElementById("f-id");
  var fName        = document.getElementById("f-name");
  var originalId   = document.getElementById("original-id");
  var warnIdChg    = document.getElementById("warn-id-change");
  var kindRadios   = document.querySelectorAll('input[name="kind"]');
  var timeModeRdos = document.querySelectorAll('input[name="time_mode"]');

  var fieldsTimeOfDay  = document.getElementById("fields-time-of-day");
  var fTimeOfDay       = document.getElementById("f-time-of-day");
  var fieldsEndDate    = document.getElementById("fields-end-date");
  var fStartDate       = document.getElementById("f-start-date");
  var fEndDate         = document.getElementById("f-end-date");
  var startDateLabel   = document.getElementById("start-date-label");
  var startDateHint    = document.getElementById("start-date-hint");
  var fNotes           = document.getElementById("f-notes");

  // -----------------------------------------------------------------------
  // Utilities
  // -----------------------------------------------------------------------
  function escHtml(s) {
    return String(s)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  }

  function showStatus(msg, kind) {
    formStatus.textContent = msg;
    formStatus.className = "status" + (kind ? " " + kind : "");
  }

  function showErr(id, msg) {
    var el = document.getElementById("err-" + id);
    if (!el) return;
    el.textContent = msg;
    el.hidden = !msg;
    // Mark input/textarea as invalid
    var input = document.querySelector('[name="' + id.replace(/-/g, "_") + '"]');
    if (!input) {
      // Try the element id mapping (dashes become hyphens in id)
      input = document.getElementById("f-" + id);
    }
    if (input) {
      if (msg) input.classList.add("invalid");
      else     input.classList.remove("invalid");
    }
  }

  function clearErrors() {
    var errs = calForm.querySelectorAll(".field-error");
    for (var i = 0; i < errs.length; i++) {
      errs[i].textContent = "";
      errs[i].hidden = true;
    }
    var invalids = calForm.querySelectorAll(".invalid");
    for (var j = 0; j < invalids.length; j++) {
      invalids[j].classList.remove("invalid");
    }
    showStatus("");
  }

  function currentKind() {
    for (var i = 0; i < kindRadios.length; i++) {
      if (kindRadios[i].checked) return kindRadios[i].value;
    }
    return "single_date";
  }

  function currentTimeMode() {
    for (var i = 0; i < timeModeRdos.length; i++) {
      if (timeModeRdos[i].checked) return timeModeRdos[i].value;
    }
    return "all_day";
  }

  // Format a calendar entry's date/time for the table summary column.
  function formatDetail(entry) {
    var parts = [];
    if (entry.kind === "annual") {
      // Show month/day only from start_date
      var mm = entry.start_date ? entry.start_date.slice(5) : "?";
      parts.push("every " + mm);
    } else {
      if (entry.start_date) parts.push(entry.start_date);
      if (entry.kind === "date_range" && entry.end_date) {
        parts.push("to " + entry.end_date);
      }
    }
    if (entry.time_mode === "specific" && entry.time_of_day) {
      parts.push("at " + entry.time_of_day);
    } else {
      parts.push("all day");
    }
    return parts.join(" ");
  }

  // -----------------------------------------------------------------------
  // Kind / time-mode switching
  // -----------------------------------------------------------------------
  function applyKindSwitch(kind) {
    fieldsEndDate.hidden = (kind !== "date_range");
    if (kind === "annual") {
      startDateLabel.textContent = "Month / day";
      startDateHint.textContent  = "Only the month and day are used. Avoid Feb 29 — use Feb 28 or Mar 1 instead.";
    } else {
      startDateLabel.textContent = "Start date";
      startDateHint.textContent  = kind === "date_range" ? "Must be on or before end date." : "";
    }
  }

  function applyTimeModeSwitch(mode) {
    fieldsTimeOfDay.hidden = (mode !== "specific");
  }

  for (var ri = 0; ri < kindRadios.length; ri++) {
    kindRadios[ri].addEventListener("change", function () {
      applyKindSwitch(currentKind());
    });
  }
  for (var ti = 0; ti < timeModeRdos.length; ti++) {
    timeModeRdos[ti].addEventListener("change", function () {
      applyTimeModeSwitch(currentTimeMode());
    });
  }

  // -----------------------------------------------------------------------
  // ID-change warning
  // -----------------------------------------------------------------------
  fId.addEventListener("input", function () {
    if (editingId && fId.value !== editingId) {
      warnIdChg.hidden = false;
    } else {
      warnIdChg.hidden = true;
    }
  });

  // -----------------------------------------------------------------------
  // Render table
  // -----------------------------------------------------------------------
  function renderTable() {
    var count = entries.length;
    calCount.textContent = count + " / " + CALENDAR_MAX;

    var atCap = count >= CALENDAR_MAX;
    btnNew.disabled = atCap;
    capNote.hidden  = !atCap;

    if (count === 0) {
      calTbody.innerHTML =
        '<tr><td colspan="4" class="table-empty">No calendar entries defined. Click "+ New entry" to create one.</td></tr>';
      return;
    }

    var html = "";
    for (var i = 0; i < entries.length; i++) {
      var e = entries[i];
      html +=
        '<tr data-id="' + escHtml(e.id) + '">' +
        '<td>' + escHtml(e.name) + '</td>' +
        '<td class="col-kind">' + escHtml(e.kind.replace(/_/g, " ")) + '</td>' +
        '<td>' + escHtml(formatDetail(e)) + '</td>' +
        '<td class="col-actions">' +
        '<div class="row-actions">' +
        '<button type="button" class="btn-edit" data-id="' + escHtml(e.id) + '"' +
        ' aria-label="Edit calendar entry: ' + escHtml(e.name) + '">Edit</button>' +
        '<button type="button" class="btn-delete" data-id="' + escHtml(e.id) + '" data-name="' + escHtml(e.name) + '"' +
        ' aria-label="Delete calendar entry: ' + escHtml(e.name) + '">Delete</button>' +
        '</div></td>' +
        '</tr>';
    }
    calTbody.innerHTML = html;

    var editBtns   = calTbody.querySelectorAll(".btn-edit");
    var deleteBtns = calTbody.querySelectorAll(".btn-delete");
    for (var ei = 0; ei < editBtns.length; ei++) {
      editBtns[ei].addEventListener("click", onEditClick);
    }
    for (var di = 0; di < deleteBtns.length; di++) {
      deleteBtns[di].addEventListener("click", onDeleteClick);
    }
  }

  // -----------------------------------------------------------------------
  // Form show/hide
  // -----------------------------------------------------------------------
  function setKindRadio(val) {
    for (var i = 0; i < kindRadios.length; i++) {
      kindRadios[i].checked = (kindRadios[i].value === val);
    }
  }

  function setTimeModeRadio(val) {
    for (var i = 0; i < timeModeRdos.length; i++) {
      timeModeRdos[i].checked = (timeModeRdos[i].value === val);
    }
  }

  function showForm(entry) {
    clearErrors();
    if (entry) {
      editingId = entry.id;
      formHeading.textContent = "Edit calendar entry";
      btnSubmit.textContent   = "Save changes";
      fId.value   = entry.id;
      fName.value = entry.name;
      originalId.value = entry.id;
      warnIdChg.hidden = true;

      setKindRadio(entry.kind);
      applyKindSwitch(entry.kind);
      setTimeModeRadio(entry.time_mode || "all_day");
      applyTimeModeSwitch(entry.time_mode || "all_day");

      fStartDate.value  = entry.start_date || "";
      fEndDate.value    = entry.end_date   || "";
      fTimeOfDay.value  = entry.time_of_day ? entry.time_of_day.slice(0, 5) : "";
      fNotes.value      = entry.notes || "";
    } else {
      editingId = null;
      formHeading.textContent = "New calendar entry";
      btnSubmit.textContent   = "Save entry";
      calForm.reset();
      originalId.value = "";
      warnIdChg.hidden = true;
      setKindRadio("single_date");
      applyKindSwitch("single_date");
      setTimeModeRadio("all_day");
      applyTimeModeSwitch("all_day");
    }
    formSection.removeAttribute("hidden");
    formSection.scrollIntoView({ behavior: "smooth", block: "start" });
    fId.focus();
  }

  function hideForm() {
    formSection.setAttribute("hidden", "");
    editingId = null;
  }

  // -----------------------------------------------------------------------
  // Edit / Delete
  // -----------------------------------------------------------------------
  function onEditClick(ev) {
    var id = ev.currentTarget.dataset.id;
    var entry = null;
    for (var i = 0; i < entries.length; i++) {
      if (entries[i].id === id) { entry = entries[i]; break; }
    }
    if (entry) showForm(entry);
  }

  function onDeleteClick(ev) {
    var btn  = ev.currentTarget;
    var id   = btn.dataset.id;
    var name = btn.dataset.name;

    if (!window.confirm("Delete calendar entry "" + name + ""?")) return;

    fetch("calendar?id=" + encodeURIComponent(id), {
      method: "DELETE",
      credentials: "same-origin"
    })
      .then(function (resp) {
        if (!resp.ok) {
          return resp.json().then(function (body) {
            throw new Error(body.message || ("HTTP " + resp.status));
          }, function () {
            throw new Error("HTTP " + resp.status);
          });
        }
        return resp.json();
      })
      .then(function (data) {
        entries = data.entries || [];
        renderTable();
        if (editingId === id) hideForm();
      })
      .catch(function (err) {
        alert("Delete failed: " + err.message);
      });
  }

  // -----------------------------------------------------------------------
  // Validation
  // -----------------------------------------------------------------------
  var ID_RE = /^[a-z0-9_]{1,32}$/;

  function validateForm() {
    clearErrors();
    var ok = true;
    var kind     = currentKind();
    var timeMode = currentTimeMode();

    var id = fId.value.trim();
    if (!ID_RE.test(id)) {
      showErr("id", "ID must be 1–32 lowercase letters, digits, or underscores.");
      ok = false;
    }
    if (!fName.value.trim()) {
      showErr("name", "Name is required.");
      ok = false;
    }

    var start = fStartDate.value;
    if (!start) {
      showErr("start-date", "A date is required.");
      ok = false;
    }

    // Annual — reject Feb 29
    if (kind === "annual" && start) {
      var mmdd = start.slice(5); // "MM-DD"
      if (mmdd === "02-29") {
        showErr("start-date", "Feb 29 is not allowed for annual entries — use Feb 28 or Mar 1.");
        ok = false;
      }
    }

    if (kind === "date_range") {
      var end = fEndDate.value;
      if (!end) {
        showErr("end-date", "End date is required for a date range.");
        ok = false;
      } else if (start && end < start) {
        showErr("end-date", "End date must be on or after start date.");
        ok = false;
      }
    }

    if (timeMode === "specific" && !fTimeOfDay.value) {
      showErr("time-of-day", "Time of day is required for specific-time entries.");
      ok = false;
    }

    return ok;
  }

  // -----------------------------------------------------------------------
  // Build entry object from form
  // -----------------------------------------------------------------------
  function buildEntryFromForm() {
    var kind     = currentKind();
    var timeMode = currentTimeMode();

    var obj = {
      id:        fId.value.trim(),
      name:      fName.value.trim(),
      kind:      kind,
      time_mode: timeMode,
      start_date:fStartDate.value,
      notes:     fNotes.value.trim()
    };

    if (timeMode === "specific") {
      // Ensure HH:MM:SS format
      var t = fTimeOfDay.value;
      if (t.length === 5) t += ":00"; // <input type="time"> may give HH:MM
      obj.time_of_day = t;
    }

    if (kind === "date_range") {
      obj.end_date = fEndDate.value;
    }

    return obj;
  }

  // -----------------------------------------------------------------------
  // Form submit
  // -----------------------------------------------------------------------
  calForm.addEventListener("submit", function (ev) {
    ev.preventDefault();
    if (!validateForm()) return;

    var entry = buildEntryFromForm();
    showStatus("Saving…");
    btnSubmit.disabled = true;

    fetch("calendar", {
      method: "POST",
      credentials: "same-origin",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ mode: "upsert", entry: entry })
    })
      .then(function (resp) {
        if (!resp.ok) {
          return resp.json().then(function (body) {
            throw new Error(body.message || ("HTTP " + resp.status));
          }, function () {
            throw new Error("HTTP " + resp.status);
          });
        }
        return resp.json();
      })
      .then(function (data) {
        entries = data.entries || [];
        renderTable();
        hideForm();
        showStatus("Saved.", "success");
      })
      .catch(function (err) {
        showStatus("Save failed: " + err.message, "error");
        btnSubmit.disabled = false;
      });
  });

  btnNew.addEventListener("click", function () {
    showForm(null);
  });

  btnCancel.addEventListener("click", hideForm);

  // -----------------------------------------------------------------------
  // Pre-populate from query string ?id=
  // -----------------------------------------------------------------------
  function getQueryId() {
    var search = window.location.search;
    if (!search) return null;
    var m = search.match(/(?:\?|&)id=([^&]*)/);
    return m ? decodeURIComponent(m[1]) : null;
  }

  // -----------------------------------------------------------------------
  // Initial load
  // -----------------------------------------------------------------------
  function loadCalendar() {
    fetch("calendar", { credentials: "same-origin" })
      .then(function (resp) {
        if (!resp.ok) throw new Error("HTTP " + resp.status);
        return resp.json();
      })
      .then(function (data) {
        entries = data.entries || [];
        renderTable();

        var qid = getQueryId();
        if (qid) {
          var target = null;
          for (var i = 0; i < entries.length; i++) {
            if (entries[i].id === qid) { target = entries[i]; break; }
          }
          if (target) showForm(target);
        }
      })
      .catch(function (err) {
        calTbody.innerHTML =
          '<tr><td colspan="4" class="table-empty">Could not load calendar: ' +
          escHtml(err.message) + '</td></tr>';
      });
  }

  loadCalendar();
})();
