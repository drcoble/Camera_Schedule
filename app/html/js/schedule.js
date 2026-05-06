// Vanilla JS — no frameworks, no CDN (DL-08).
// GET  /local/camera_schedule/events_today?lookahead_days=1  → schedule list
// POST /local/camera_schedule/events  body {id, enabled}     → toggle result

(function () {
  "use strict";

  // -----------------------------------------------------------------------
  // State
  // -----------------------------------------------------------------------
  var rows = [];          // flat array of row objects from the last fetch
  var searchQuery = "";   // current filter string (lowercase)

  // Map category → section element ids
  var SECTIONS = [
    { category: "solar",    secId: "sec-solar",    bodyId: "body-solar",    countId: "count-solar" },
    { category: "lunar",    secId: "sec-lunar",    bodyId: "body-lunar",    countId: "count-lunar" },
    { category: "seasonal", secId: "sec-seasonal", bodyId: "body-seasonal", countId: "count-seasonal" },
    { category: "anchor",   secId: "sec-anchor",   bodyId: "body-anchor",   countId: "count-anchor" },
    { category: "calendar", secId: "sec-calendar", bodyId: "body-calendar", countId: "count-calendar" }
  ];

  var REASON_LABELS = {
    "disabled":          "not firing today — disabled",
    "polar_no_event":    "not firing today — polar region",
    "lunar_no_event":    "not firing today — no lunar event",
    "solar_no_event":    "not firing today — no solar event",
    "out_of_range":      "not firing today — out of range",
    "dependency_missing":"not firing today — source missing",
    "dependency_cycle":  "not firing today — dependency cycle",
    "threshold_unmet":   "not firing today — threshold unmet"
  };

  // -----------------------------------------------------------------------
  // DOM refs
  // -----------------------------------------------------------------------
  var searchInput  = document.getElementById("search");
  var pageStatus   = document.getElementById("page-status");
  var btnRefresh   = document.getElementById("btn-refresh");

  // -----------------------------------------------------------------------
  // Utilities
  // -----------------------------------------------------------------------
  function showStatus(msg, kind) {
    pageStatus.textContent = msg;
    pageStatus.className = "status page-status" + (kind ? " " + kind : "");
  }

  // Format an ISO-8601 local-time string as HH:MM (just the time portion,
  // readable at a glance). The local field already carries the offset so
  // we parse the time part directly rather than relying on Date.
  function formatTime(isoLocal) {
    if (!isoLocal) return "";
    // e.g. "2026-05-06T06:42:13-04:00" — grab the T…  segment
    var m = isoLocal.match(/T(\d{2}:\d{2})/);
    return m ? m[1] : isoLocal;
  }

  // Format a date+time as "May 31 18:14"
  function formatDateTime(isoLocal) {
    if (!isoLocal) return "";
    var m = isoLocal.match(/^(\d{4})-(\d{2})-(\d{2})T(\d{2}:\d{2})/);
    if (!m) return isoLocal;
    var monthNames = ["Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"];
    var month = monthNames[parseInt(m[2], 10) - 1];
    var day   = parseInt(m[3], 10);
    return month + " " + day + " " + m[4];
  }

  // Whether a row's fire date is the same local calendar day as today.
  // We compare the date portion of next_fire_local.
  function isSameDay(isoLocal) {
    if (!isoLocal) return false;
    var today = (new Date()).toISOString().slice(0, 10); // UTC date — good enough for same-day display decision
    return isoLocal.slice(0, 10) === today;
  }

  // Build the time cell content for a row object.
  function buildTimeContent(row) {
    if (row.not_firing_today) {
      var reason = row.not_firing_reason || "not_firing_today";
      var label  = REASON_LABELS[reason] || ("not firing today — " + reason);
      return '<span class="row-not-firing">' + escHtml(label) + "</span>";
    }
    if (!row.next_fire_local) {
      return '<span class="row-not-firing">no fire scheduled</span>';
    }
    var useDateTime = !isSameDay(row.next_fire_local);
    var fireStr = useDateTime ? formatDateTime(row.next_fire_local) : formatTime(row.next_fire_local);
    var html = '<span class="row-time-main">' + escHtml(fireStr) + " local</span>";
    if (row.stateful && row.next_end_local) {
      var endStr = useDateTime ? formatDateTime(row.next_end_local) : formatTime(row.next_end_local);
      html += '<br><span class="row-time-end">→ ' + escHtml(endStr) + " local</span>";
    }
    return html;
  }

  function escHtml(s) {
    return String(s)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  }

  var LOCK_SVG = '<svg class="lock-glyph" viewBox="0 0 16 16" aria-hidden="true">' +
    '<rect x="3" y="7" width="10" height="8" rx="1.5"/>' +
    '<path d="M5 7V5a3 3 0 0 1 6 0v2"/>' +
    '</svg>';

  // -----------------------------------------------------------------------
  // Render
  // -----------------------------------------------------------------------
  function render() {
    SECTIONS.forEach(function (sec) {
      var body = document.getElementById(sec.bodyId);
      body.innerHTML = "";

      var sectionRows = rows.filter(function (r) { return r.category === sec.category; });
      var matchedRows = sectionRows.filter(function (r) {
        return searchQuery === "" || r.name.toLowerCase().indexOf(searchQuery) !== -1;
      });

      // Update section header counts (always against full section, not filtered)
      var enabledCount = sectionRows.filter(function (r) { return r.enabled; }).length;
      document.getElementById(sec.countId).textContent =
        enabledCount + " enabled / " + sectionRows.length + " total";

      var isBuiltin = (sec.category === "solar" || sec.category === "lunar" || sec.category === "seasonal");
      var editPage  = sec.category === "calendar" ? "calendar.html" : "anchors.html";

      matchedRows.forEach(function (row) {
        var el = document.createElement("div");
        el.className = "schedule-row";
        el.setAttribute("role", "listitem");
        el.dataset.id = row.id;

        var checkedAttr = row.enabled ? "true" : "false";
        var toggleHtml =
          '<button type="button" class="toggle-btn" role="switch"' +
          ' aria-checked="' + checkedAttr + '"' +
          ' aria-label="' + escHtml(row.name) + (row.enabled ? " (enabled)" : " (disabled)") + '"' +
          ' data-id="' + escHtml(row.id) + '">' +
          "</button>";

        var nameHtml =
          '<div class="row-name">' +
          '<span class="row-name-text">' + escHtml(row.name) + "</span>" +
          (isBuiltin ? LOCK_SVG : "") +
          "</div>";

        var timeHtml = '<div class="row-time">' + buildTimeContent(row) + "</div>";

        var indicatorHtml = '<span class="toggle-indicator" aria-live="polite"></span>';

        var actionsHtml = "";
        if (!isBuiltin) {
          actionsHtml =
            '<div class="row-actions">' +
            '<a href="' + editPage + '?id=' + encodeURIComponent(row.id) + '">Edit</a>' +
            '<button type="button" class="btn-delete" data-id="' + escHtml(row.id) + '" data-name="' + escHtml(row.name) + '"' +
            ' data-category="' + escHtml(row.category) + '">Delete</button>' +
            "</div>";
        }

        el.innerHTML = toggleHtml + nameHtml + timeHtml + indicatorHtml + actionsHtml;
        body.appendChild(el);
      });

      // Auto-collapse section when filter active and no matches
      var secEl  = document.getElementById(sec.secId);
      var hdrEl  = document.getElementById("hdr-" + sec.category);
      var bodyEl = document.getElementById(sec.bodyId);

      if (searchQuery !== "" && matchedRows.length === 0) {
        secEl.setAttribute("hidden", "");
      } else {
        secEl.removeAttribute("hidden");
        // Restore expanded/collapsed to whatever the header says
        var expanded = hdrEl.getAttribute("aria-expanded") !== "false";
        bodyEl.hidden = !expanded;
      }
    });

    wireRowEvents();
  }

  // -----------------------------------------------------------------------
  // Wire row-level events (called after each render)
  // -----------------------------------------------------------------------
  function wireRowEvents() {
    // Toggle buttons
    var toggleBtns = document.querySelectorAll(".toggle-btn");
    for (var i = 0; i < toggleBtns.length; i++) {
      toggleBtns[i].addEventListener("click", onToggleClick);
    }

    // Delete buttons
    var deleteBtns = document.querySelectorAll(".btn-delete");
    for (var j = 0; j < deleteBtns.length; j++) {
      deleteBtns[j].addEventListener("click", onDeleteClick);
    }
  }

  // -----------------------------------------------------------------------
  // Toggle handler
  // -----------------------------------------------------------------------
  function onToggleClick(ev) {
    var btn      = ev.currentTarget;
    var id       = btn.dataset.id;
    var current  = btn.getAttribute("aria-checked") === "true";
    var newValue = !current;

    // Find the row object
    var rowData = null;
    for (var i = 0; i < rows.length; i++) {
      if (rows[i].id === id) { rowData = rows[i]; break; }
    }
    if (!rowData) return;

    // Optimistic UI update
    btn.setAttribute("aria-checked", newValue ? "true" : "false");
    btn.setAttribute("aria-label", rowData.name + (newValue ? " (enabled)" : " (disabled)"));
    rowData.enabled = newValue;

    // Show saving indicator
    var indicator = btn.closest(".schedule-row").querySelector(".toggle-indicator");
    indicator.textContent = "Saving…";
    indicator.className = "toggle-indicator toggle-saving";

    btn.disabled = true;

    fetch("events", {
      method: "POST",
      credentials: "same-origin",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ id: id, enabled: newValue })
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
      .then(function () {
        indicator.textContent = "";
        indicator.className = "toggle-indicator";
        btn.disabled = false;
        // If toggled on, refresh so next_fire times are populated
        if (newValue) {
          loadSchedules();
        } else {
          // Toggled off — update row display in-place
          rowData.not_firing_today = true;
          rowData.not_firing_reason = "disabled";
          rowData.next_fire_local = null;
          rowData.next_fire_utc = null;
          rowData.next_end_local = null;
          rowData.next_end_utc = null;
          render();
        }
      })
      .catch(function (err) {
        // Revert
        btn.setAttribute("aria-checked", current ? "true" : "false");
        btn.setAttribute("aria-label", rowData.name + (current ? " (enabled)" : " (disabled)"));
        rowData.enabled = current;
        btn.disabled = false;

        indicator.className = "toggle-indicator toggle-failed";
        indicator.textContent = "Failed — retry?";
        indicator.title = err.message || "Unknown error";

        // Clicking the indicator re-submits
        indicator.onclick = function () {
          indicator.onclick = null;
          btn.click();
        };
      });
  }

  // -----------------------------------------------------------------------
  // Delete handler
  // -----------------------------------------------------------------------
  function onDeleteClick(ev) {
    var btn      = ev.currentTarget;
    var id       = btn.dataset.id;
    var name     = btn.dataset.name;
    var category = btn.dataset.category;

    // Lazy-fetch anchors to check for dangling references.
    // For calendar entries the referencing anchors list is fetched the same way.
    fetchReferenceCount(id, category, function (refCount) {
      var msg = "Delete “" + name + "”?";
      if (refCount > 0) {
        msg += "\n\n" + refCount + " other anchor" + (refCount === 1 ? " references" : "s reference") +
               " this entry. Deleting it will cause those anchors to skip firing until edited.";
      }
      if (!window.confirm(msg)) return;

      var endpoint = category === "calendar" ? "calendar" : "anchors";
      fetch(endpoint + "?id=" + encodeURIComponent(id), {
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
        .then(function () {
          // Remove from local rows and re-render
          rows = rows.filter(function (r) { return r.id !== id; });
          render();
        })
        .catch(function (err) {
          showStatus("Delete failed: " + err.message, "error");
        });
    });
  }

  // Fetch anchor list and count references to the given id.
  // Calls back with the count (0 if fetch fails or category is calendar not anchor).
  function fetchReferenceCount(id, category, cb) {
    // Only anchors can be event_sources; calendar entries can also be sources.
    // Either way we scan operator anchors for references.
    fetch("anchors", { credentials: "same-origin" })
      .then(function (resp) {
        if (!resp.ok) { cb(0); return null; }
        return resp.json();
      })
      .then(function (data) {
        if (!data) return;
        var count = 0;
        var ops = data.operator || [];
        for (var i = 0; i < ops.length; i++) {
          var a = ops[i];
          if (a.id === id) continue; // the item itself
          if (a.event_source === id) count++;
          if (a.start_event === id) count++;
          if (a.end_event === id) count++;
        }
        cb(count);
      })
      .catch(function () { cb(0); });
  }

  // -----------------------------------------------------------------------
  // Section collapse/expand
  // -----------------------------------------------------------------------
  SECTIONS.forEach(function (sec) {
    var hdrBtn = document.getElementById("hdr-" + sec.category);
    var bodyEl = document.getElementById(sec.bodyId);

    hdrBtn.addEventListener("click", function () {
      var expanded = hdrBtn.getAttribute("aria-expanded") !== "false";
      expanded = !expanded;
      hdrBtn.setAttribute("aria-expanded", expanded ? "true" : "false");
      bodyEl.hidden = !expanded;
    });
  });

  // -----------------------------------------------------------------------
  // Search
  // -----------------------------------------------------------------------
  searchInput.addEventListener("input", function () {
    searchQuery = searchInput.value.trim().toLowerCase();
    render();
  });

  // -----------------------------------------------------------------------
  // Data load
  // -----------------------------------------------------------------------
  function loadSchedules() {
    showStatus("Loading…");
    fetch("events_today?lookahead_days=1", { credentials: "same-origin" })
      .then(function (resp) {
        if (!resp.ok) throw new Error("HTTP " + resp.status);
        return resp.json();
      })
      .then(function (data) {
        rows = data.rows || [];
        showStatus("");
        render();
      })
      .catch(function (err) {
        showStatus("Could not load schedules: " + err.message, "error");
      });
  }

  btnRefresh.addEventListener("click", loadSchedules);

  loadSchedules();
})();
