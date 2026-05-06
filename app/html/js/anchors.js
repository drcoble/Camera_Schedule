// Vanilla JS — no frameworks, no CDN (DL-08).
// GET    /local/camera_schedule/anchors        → {built_in, operator}
// POST   /local/camera_schedule/anchors        body {mode:"upsert", anchor:{…}}
// DELETE /local/camera_schedule/anchors?id=…   → updated list

(function () {
  "use strict";

  var ANCHORS_MAX = 64;

  // -----------------------------------------------------------------------
  // State
  // -----------------------------------------------------------------------
  var operatorAnchors = []; // current operator anchor list
  var allSources      = []; // [{id, name}] for event_source dropdowns (built-ins + operator)
  var editingId       = null; // null = creating new

  // -----------------------------------------------------------------------
  // DOM refs
  // -----------------------------------------------------------------------
  var anchorCount  = document.getElementById("anchor-count");
  var capNote      = document.getElementById("cap-note");
  var anchorTbody  = document.getElementById("anchor-tbody");
  var btnNew       = document.getElementById("btn-new");
  var formSection  = document.getElementById("form-section");
  var formHeading  = document.getElementById("form-heading");
  var anchorForm   = document.getElementById("anchor-form");
  var formStatus   = document.getElementById("form-status");
  var btnSubmit    = document.getElementById("btn-submit");
  var btnCancel    = document.getElementById("btn-cancel");

  var fId         = document.getElementById("f-id");
  var fName       = document.getElementById("f-name");
  var originalId  = document.getElementById("original-id");
  var warnIdChg   = document.getElementById("warn-id-change");
  var kindRadios  = document.querySelectorAll('input[name="kind"]');

  var fieldsOffset    = document.getElementById("fields-offset");
  var fieldsPaired    = document.getElementById("fields-paired");
  var fieldsThreshold = document.getElementById("fields-threshold");

  var fEventSource    = document.getElementById("f-event-source");
  var fOffsetMinutes  = document.getElementById("f-offset-minutes");
  var fDurationMinutes= document.getElementById("f-duration-minutes");
  var fStartEvent     = document.getElementById("f-start-event");
  var fStartOffset    = document.getElementById("f-start-offset");
  var fEndEvent       = document.getElementById("f-end-event");
  var fEndOffset      = document.getElementById("f-end-offset");
  var fOp             = document.getElementById("f-op");
  var fValue          = document.getElementById("f-value");

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
    var input = document.getElementById("f-" + id.replace(/-/g, "-"));
    if (input) {
      if (msg) input.classList.add("invalid");
      else     input.classList.remove("invalid");
    }
  }

  function clearErrors() {
    var errs = anchorForm.querySelectorAll(".field-error");
    for (var i = 0; i < errs.length; i++) {
      errs[i].textContent = "";
      errs[i].hidden = true;
    }
    var invalids = anchorForm.querySelectorAll(".invalid");
    for (var j = 0; j < invalids.length; j++) {
      invalids[j].classList.remove("invalid");
    }
    showStatus("");
  }

  function currentKind() {
    for (var i = 0; i < kindRadios.length; i++) {
      if (kindRadios[i].checked) return kindRadios[i].value;
    }
    return "offset";
  }

  function getSelectedValue(selectEl) {
    return selectEl.options[selectEl.selectedIndex].value;
  }

  // Build source dropdown options HTML
  function buildSourceOptions(selectedId) {
    var html = '<option value="">— select event —</option>';
    for (var i = 0; i < allSources.length; i++) {
      var s = allSources[i];
      var sel = s.id === selectedId ? ' selected' : '';
      html += '<option value="' + escHtml(s.id) + '"' + sel + '>' + escHtml(s.name) + '</option>';
    }
    return html;
  }

  function populateSourceDropdowns(selectedSource, selectedStart, selectedEnd) {
    var src = buildSourceOptions(selectedSource || "");
    var sta = buildSourceOptions(selectedStart  || "");
    var end = buildSourceOptions(selectedEnd    || "");
    fEventSource.innerHTML = src;
    fStartEvent.innerHTML  = sta;
    fEndEvent.innerHTML    = end;
  }

  // -----------------------------------------------------------------------
  // Kind switching
  // -----------------------------------------------------------------------
  function switchKind(kind) {
    fieldsOffset.hidden    = (kind !== "offset");
    fieldsPaired.hidden    = (kind !== "paired");
    fieldsThreshold.hidden = (kind !== "threshold");
  }

  for (var ri = 0; ri < kindRadios.length; ri++) {
    kindRadios[ri].addEventListener("change", function () {
      switchKind(currentKind());
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
    var count = operatorAnchors.length;
    anchorCount.textContent = count + " / " + ANCHORS_MAX;

    var atCap = count >= ANCHORS_MAX;
    btnNew.disabled = atCap;
    capNote.hidden  = !atCap;

    if (count === 0) {
      anchorTbody.innerHTML =
        '<tr><td colspan="4" class="table-empty">No operator anchors defined. Click "+ New anchor" to create one.</td></tr>';
      return;
    }

    var html = "";
    for (var i = 0; i < operatorAnchors.length; i++) {
      var a = operatorAnchors[i];
      var detail = buildDetail(a);
      html +=
        '<tr data-id="' + escHtml(a.id) + '">' +
        '<td>' + escHtml(a.name) + '</td>' +
        '<td class="col-kind">' + escHtml(a.kind) + '</td>' +
        '<td>' + detail + '</td>' +
        '<td class="col-actions">' +
        '<div class="row-actions">' +
        '<button type="button" class="btn-edit" data-id="' + escHtml(a.id) + '">Edit</button>' +
        '<button type="button" class="btn-delete" data-id="' + escHtml(a.id) + '" data-name="' + escHtml(a.name) + '">Delete</button>' +
        '</div></td>' +
        '</tr>';
    }
    anchorTbody.innerHTML = html;

    // Wire events
    var editBtns = anchorTbody.querySelectorAll(".btn-edit");
    for (var ei = 0; ei < editBtns.length; ei++) {
      editBtns[ei].addEventListener("click", onEditClick);
    }
    var deleteBtns = anchorTbody.querySelectorAll(".btn-delete");
    for (var di = 0; di < deleteBtns.length; di++) {
      deleteBtns[di].addEventListener("click", onDeleteClick);
    }
  }

  function buildDetail(a) {
    if (a.kind === "offset") {
      var off = a.offset_minutes > 0 ? "+" + a.offset_minutes : String(a.offset_minutes);
      var dur = a.duration_minutes > 0 ? ", " + a.duration_minutes + " min duration" : "";
      return escHtml(a.event_source + " " + off + " min" + dur);
    }
    if (a.kind === "paired") {
      return escHtml(a.start_event + " → " + a.end_event);
    }
    if (a.kind === "threshold") {
      return escHtml("Moon illumination " + a.op + " " + a.value);
    }
    return "";
  }

  // -----------------------------------------------------------------------
  // Form show/hide
  // -----------------------------------------------------------------------
  function showForm(anchor) {
    clearErrors();
    if (anchor) {
      // Edit mode
      editingId = anchor.id;
      formHeading.textContent = "Edit anchor";
      btnSubmit.textContent   = "Save changes";
      fId.value   = anchor.id;
      fName.value = anchor.name;
      originalId.value = anchor.id;
      warnIdChg.hidden = true;

      // Set kind
      for (var ki = 0; ki < kindRadios.length; ki++) {
        kindRadios[ki].checked = (kindRadios[ki].value === anchor.kind);
      }
      switchKind(anchor.kind);

      if (anchor.kind === "offset") {
        populateSourceDropdowns(anchor.event_source, "", "");
        fOffsetMinutes.value   = anchor.offset_minutes;
        fDurationMinutes.value = anchor.duration_minutes;
      } else if (anchor.kind === "paired") {
        populateSourceDropdowns("", anchor.start_event, anchor.end_event);
        fStartOffset.value = anchor.start_offset_minutes;
        fEndOffset.value   = anchor.end_offset_minutes;
      } else if (anchor.kind === "threshold") {
        fOp.value    = anchor.op;
        fValue.value = anchor.value;
      }
    } else {
      // Create mode
      editingId = null;
      formHeading.textContent = "New anchor";
      btnSubmit.textContent   = "Save anchor";
      anchorForm.reset();
      originalId.value = "";
      warnIdChg.hidden = true;
      switchKind("offset");
      populateSourceDropdowns("", "", "");
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
    var anchor = null;
    for (var i = 0; i < operatorAnchors.length; i++) {
      if (operatorAnchors[i].id === id) { anchor = operatorAnchors[i]; break; }
    }
    if (anchor) showForm(anchor);
  }

  function onDeleteClick(ev) {
    var btn  = ev.currentTarget;
    var id   = btn.dataset.id;
    var name = btn.dataset.name;

    // Check references
    var refCount = 0;
    for (var i = 0; i < operatorAnchors.length; i++) {
      var a = operatorAnchors[i];
      if (a.id === id) continue;
      if (a.event_source === id) refCount++;
      if (a.start_event === id)  refCount++;
      if (a.end_event === id)    refCount++;
    }

    var msg = "Delete anchor "" + name + ""?";
    if (refCount > 0) {
      msg += "\n\n" + refCount + " other anchor" + (refCount === 1 ? " references" : "s reference") +
             " this anchor. Deleting it will cause those anchors to skip firing until edited.";
    }
    if (!window.confirm(msg)) return;

    fetch("anchors?id=" + encodeURIComponent(id), {
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
        operatorAnchors = data.operator || [];
        buildSourceList(data.built_in || [], data.operator || []);
        renderTable();
        if (editingId === id) hideForm();
      })
      .catch(function (err) {
        alert("Delete failed: " + err.message);
      });
  }

  // -----------------------------------------------------------------------
  // Client-side validation
  // -----------------------------------------------------------------------
  var ID_RE = /^[a-z0-9_]{1,32}$/;

  function validateForm() {
    clearErrors();
    var ok = true;
    var kind = currentKind();

    var id = fId.value.trim();
    if (!ID_RE.test(id)) {
      showErr("id", "ID must be 1–32 lowercase letters, digits, or underscores.");
      ok = false;
    }

    if (!fName.value.trim()) {
      showErr("name", "Name is required.");
      ok = false;
    }

    if (kind === "offset") {
      if (!getSelectedValue(fEventSource)) {
        showErr("event-source", "Select a source event.");
        ok = false;
      }
      var off = parseInt(fOffsetMinutes.value, 10);
      if (isNaN(off) || off < -1440 || off > 1440) {
        showErr("offset-minutes", "Offset must be between −1440 and 1440.");
        ok = false;
      }
      var dur = parseInt(fDurationMinutes.value, 10);
      if (isNaN(dur) || dur < 0 || dur > 1440) {
        showErr("duration-minutes", "Duration must be between 0 and 1440.");
        ok = false;
      }
    }

    if (kind === "paired") {
      if (!getSelectedValue(fStartEvent)) {
        showErr("start-event", "Select a start event.");
        ok = false;
      }
      if (!getSelectedValue(fEndEvent)) {
        showErr("end-event", "Select an end event.");
        ok = false;
      }
    }

    if (kind === "threshold") {
      var val = parseFloat(fValue.value);
      if (isNaN(val) || val < 0 || val > 1) {
        showErr("value", "Value must be between 0.0 and 1.0.");
        ok = false;
      }
    }

    return ok;
  }

  // -----------------------------------------------------------------------
  // Build anchor object from form
  // -----------------------------------------------------------------------
  function buildAnchorFromForm() {
    var kind = currentKind();
    var obj = {
      id:   fId.value.trim(),
      name: fName.value.trim(),
      kind: kind
    };

    if (kind === "offset") {
      obj.event_source      = getSelectedValue(fEventSource);
      obj.offset_minutes    = parseInt(fOffsetMinutes.value, 10);
      obj.duration_minutes  = parseInt(fDurationMinutes.value, 10);
    } else if (kind === "paired") {
      obj.start_event          = getSelectedValue(fStartEvent);
      obj.start_offset_minutes = parseInt(fStartOffset.value, 10);
      obj.end_event            = getSelectedValue(fEndEvent);
      obj.end_offset_minutes   = parseInt(fEndOffset.value, 10);
    } else if (kind === "threshold") {
      obj.metric = "moon_illumination";
      obj.op     = getSelectedValue(fOp);
      obj.value  = parseFloat(fValue.value);
    }
    return obj;
  }

  // -----------------------------------------------------------------------
  // Form submit
  // -----------------------------------------------------------------------
  anchorForm.addEventListener("submit", function (ev) {
    ev.preventDefault();
    if (!validateForm()) return;

    var anchor = buildAnchorFromForm();
    showStatus("Saving…");
    btnSubmit.disabled = true;

    fetch("anchors", {
      method: "POST",
      credentials: "same-origin",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ mode: "upsert", anchor: anchor })
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
        operatorAnchors = data.operator || [];
        buildSourceList(data.built_in || [], data.operator || []);
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

  btnCancel.addEventListener("click", function () {
    hideForm();
  });

  // -----------------------------------------------------------------------
  // Build allSources from anchor list (for dropdowns)
  // -----------------------------------------------------------------------
  function buildSourceList(builtIns, operator) {
    allSources = [];
    var i;
    for (i = 0; i < builtIns.length; i++) {
      allSources.push({ id: builtIns[i].id, name: builtIns[i].name });
    }
    for (i = 0; i < operator.length; i++) {
      allSources.push({ id: operator[i].id, name: operator[i].name });
    }
  }

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
  function loadAnchors() {
    fetch("anchors", { credentials: "same-origin" })
      .then(function (resp) {
        if (!resp.ok) throw new Error("HTTP " + resp.status);
        return resp.json();
      })
      .then(function (data) {
        operatorAnchors = data.operator || [];
        buildSourceList(data.built_in || [], data.operator || []);
        renderTable();

        var qid = getQueryId();
        if (qid) {
          var target = null;
          for (var i = 0; i < operatorAnchors.length; i++) {
            if (operatorAnchors[i].id === qid) { target = operatorAnchors[i]; break; }
          }
          if (target) showForm(target);
        } else {
          formSection.setAttribute("hidden", "");
        }
      })
      .catch(function (err) {
        anchorTbody.innerHTML =
          '<tr><td colspan="4" class="table-empty">Could not load anchors: ' +
          escHtml(err.message) + '</td></tr>';
      });
  }

  loadAnchors();
})();
