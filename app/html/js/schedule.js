// Vanilla JS — no frameworks, no CDN (DL-08).
// M6: GET  /local/camera_schedule/events_today?lookahead_days=1  → schedule list
//     POST /local/camera_schedule/events  body {id, enabled}     → toggle result
// M7: GET  /local/camera_schedule/status                         → status panel
//     POST /local/camera_schedule/recompute                      → recompute now
//     GET  /local/camera_schedule/export                         → download (anchor link)
//     POST /local/camera_schedule/import                         → import modal
//     GET  /local/camera_schedule/debug                          → debug toggle state
//     POST /local/camera_schedule/debug  body {debug_logging}    → toggle debug

(function () {
  "use strict";

  // -----------------------------------------------------------------------
  // State
  // -----------------------------------------------------------------------
  var rows = [];          // flat array of row objects from the last fetch
  var searchQuery = "";   // current filter string (lowercase)
  var pollTimer = null;   // setInterval handle for status polling
  var pollInterval = 60;  // seconds, read from /status axparameters
  var recentShowAll = false;     // whether "show all 50" is active
  var recentData = [];           // last received recent[] array

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

  // Human-readable trigger label (closed enum per §1.1)
  var TRIGGER_LABELS = {
    "boot":            "boot",
    "midnight":        "midnight",
    "location_change": "location change",
    "manual":          "manual",
    "config_change":   "config change",
    "import":          "import",
    "tz_change":       "timezone change"
  };

  // -----------------------------------------------------------------------
  // DOM refs — M6
  // -----------------------------------------------------------------------
  var searchInput  = document.getElementById("search");
  var pageStatus   = document.getElementById("page-status");
  var btnRefresh   = document.getElementById("btn-refresh");

  // -----------------------------------------------------------------------
  // DOM refs — M7 status panel
  // -----------------------------------------------------------------------
  var hdrStatus         = document.getElementById("hdr-status");
  var bodyStatus        = document.getElementById("body-status");
  var statusError       = document.getElementById("status-error");
  var statusGrid        = document.getElementById("status-grid");
  var statLastTime      = document.getElementById("stat-last-time");
  var statLastTrigger   = document.getElementById("stat-last-trigger");
  var statLastDetail    = document.getElementById("stat-last-detail");
  var statNextTime      = document.getElementById("stat-next-time");
  var statNextReason    = document.getElementById("stat-next-reason");
  var statLocation      = document.getElementById("stat-location");
  var statLocationBadge = document.getElementById("stat-location-badge");
  var statTz            = document.getElementById("stat-tz");
  var cntDeclared       = document.getElementById("cnt-declared");
  var cntEnabled        = document.getElementById("cnt-enabled");
  var cntAnchors        = document.getElementById("cnt-anchors");
  var cntCalendar       = document.getElementById("cnt-calendar");
  var recentActivity    = document.getElementById("recent-activity");
  var btnRecentToggle   = document.getElementById("btn-recent-toggle");
  var recentList        = document.getElementById("recent-list");
  var recentShowAllWrap = document.getElementById("recent-show-all-wrap");
  var btnShowAllRecent  = document.getElementById("btn-show-all-recent");

  // -----------------------------------------------------------------------
  // DOM refs — M7 toolbar
  // -----------------------------------------------------------------------
  var btnRecompute = document.getElementById("btn-recompute");
  var btnExport    = document.getElementById("btn-export");
  var btnImport    = document.getElementById("btn-import");

  // -----------------------------------------------------------------------
  // DOM refs — M7 debug toggle
  // -----------------------------------------------------------------------
  var hdrDebug      = document.getElementById("hdr-debug");
  var bodyDebug     = document.getElementById("body-debug");
  var chkDebug      = document.getElementById("chk-debug");
  var debugIndicator= document.getElementById("debug-indicator");

  // -----------------------------------------------------------------------
  // DOM refs — M7 import modal
  // -----------------------------------------------------------------------
  var importModal       = document.getElementById("import-modal");
  var importModalDialog = importModal.querySelector(".modal-dialog");
  var importFile        = document.getElementById("import-file");
  var importStatus      = document.getElementById("import-status");
  var btnImportSubmit   = document.getElementById("btn-import-submit");
  var btnImportCancel   = document.getElementById("btn-import-cancel");
  var _importOpener     = null; // element to restore focus to on close

  // -----------------------------------------------------------------------
  // Utilities — shared
  // -----------------------------------------------------------------------
  function showStatus(msg, kind) {
    pageStatus.textContent = msg;
    pageStatus.className = "status page-status" + (kind ? " " + kind : "");
  }

  function escHtml(s) {
    return String(s)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  }

  // Format ISO-8601 local-time string as HH:MM
  function formatTime(isoLocal) {
    if (!isoLocal) return "";
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
    return month + " " + day + " " + m[4];
  }

  function isSameDay(isoLocal) {
    if (!isoLocal) return false;
    var today = (new Date()).toISOString().slice(0, 10);
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
      html += '<br><span class="row-time-end">→ ' + escHtml(endStr) + " local</span>";
    }
    return html;
  }

  var LOCK_SVG = '<svg class="lock-glyph" viewBox="0 0 16 16" aria-hidden="true">' +
    '<rect x="3" y="7" width="10" height="8" rx="1.5"/>' +
    '<path d="M5 7V5a3 3 0 0 1 6 0v2"/>' +
    '</svg>';

  // -----------------------------------------------------------------------
  // Relative time helpers for status panel
  // -----------------------------------------------------------------------

  // Produce "just now", "5 min ago", "2 h ago", "3 d ago" from a UTC ISO string.
  function relativeTime(isoUtc) {
    if (!isoUtc) return "—";
    var then = new Date(isoUtc).getTime();
    if (isNaN(then)) return isoUtc;
    var diffMs = Date.now() - then;
    var diffSec = Math.round(diffMs / 1000);
    if (diffSec < 5)   return "just now";
    if (diffSec < 60)  return diffSec + " s ago";
    var diffMin = Math.round(diffSec / 60);
    if (diffMin < 60)  return diffMin + " min ago";
    var diffH = Math.round(diffMin / 60);
    if (diffH < 48)    return diffH + " h ago";
    return Math.round(diffH / 24) + " d ago";
  }

  // Produce "in 6 h 12 m", "in 45 min", "in 3 d" from a future UTC ISO string.
  function relativeFuture(isoUtc) {
    if (!isoUtc) return "—";
    var then = new Date(isoUtc).getTime();
    if (isNaN(then)) return isoUtc;
    var diffMs = then - Date.now();
    if (diffMs <= 0) return "imminent";
    var diffMin = Math.round(diffMs / 60000);
    if (diffMin < 60) return "in " + diffMin + " min";
    var h = Math.floor(diffMin / 60);
    var m = diffMin % 60;
    if (h < 48) return "in " + h + " h" + (m > 0 ? " " + m + " m" : "");
    return "in " + Math.round(h / 24) + " d";
  }

  // -----------------------------------------------------------------------
  // M7: Status panel rendering
  // -----------------------------------------------------------------------

  function renderStatus(data) {
    statusError.hidden = true;
    statusGrid.hidden  = false;

    // --- Last recompute ---
    var lr = data.last_recompute;
    if (lr) {
      statLastTime.textContent  = relativeTime(lr.started_at);
      statLastTime.title        = lr.started_at || "";
      statLastTrigger.textContent = TRIGGER_LABELS[lr.trigger] || lr.trigger || "—";
      statLastDetail.textContent  = lr.events_armed + " armed / " + lr.anchors_evaluated + " evaluated" +
        (lr.elapsed_ms != null ? ", " + lr.elapsed_ms + " ms" : "");
    } else {
      statLastTime.textContent = "—";
      statLastTrigger.textContent = "";
      statLastDetail.textContent  = "";
    }

    // --- Next recompute ---
    var nr = data.next_recompute;
    if (nr) {
      statNextTime.textContent   = relativeFuture(nr.scheduled_at);
      statNextTime.title         = nr.scheduled_at || "";
      statNextReason.textContent = TRIGGER_LABELS[nr.reason] || nr.reason || "—";
    } else {
      statNextTime.textContent   = "—";
      statNextReason.textContent = "";
    }

    // --- Location ---
    var loc = data.location;
    if (loc) {
      var latStr = loc.lat != null ? loc.lat.toFixed(3) : "?";
      var lonStr = loc.lon != null ? loc.lon.toFixed(3) : "?";
      statLocation.textContent = latStr + "°, " + lonStr + "°";
      statLocationBadge.textContent = loc.valid ? "valid" : "invalid";
      statLocationBadge.className   = "validity-badge" + (loc.valid ? " badge-ok" : " badge-err");
      var tzOffset = "";
      if (loc.tz_offset_seconds != null) {
        var totalMin = Math.abs(Math.round(loc.tz_offset_seconds / 60));
        var h2 = Math.floor(totalMin / 60);
        var m2 = totalMin % 60;
        tzOffset = (loc.tz_offset_seconds < 0 ? "−" : "+") +
          String(h2).padStart(2, "0") + ":" + String(m2).padStart(2, "0");
      }
      statTz.textContent = (loc.tz || "") + (tzOffset ? " (" + tzOffset + ")" : "");
    }

    // --- Counts ---
    var c = data.counts;
    if (c) {
      cntDeclared.textContent = c.topics_declared != null ? c.topics_declared : "—";
      cntEnabled.textContent  = c.topics_enabled  != null ? c.topics_enabled  : "—";
      cntAnchors.textContent  = c.anchors_user     != null ? c.anchors_user    : "—";
      cntCalendar.textContent = c.calendar_entries != null ? c.calendar_entries : "—";
    }

    // --- Recent activity ---
    recentData = data.recent || [];
    recentActivity.hidden = (recentData.length === 0);
    renderRecentList();

    // --- Poll interval ---
    var axp = data.axparameters;
    if (axp && typeof axp.poll_interval_seconds === "number" && axp.poll_interval_seconds >= 30) {
      if (axp.poll_interval_seconds !== pollInterval) {
        pollInterval = axp.poll_interval_seconds;
        resetPollTimer();
      }
    }
  }

  function renderRecentList() {
    var maxShow = recentShowAll ? recentData.length : Math.min(10, recentData.length);
    var html = "";
    for (var i = 0; i < maxShow; i++) {
      var r = recentData[i];
      var label = TRIGGER_LABELS[r.trigger] || r.trigger || "?";
      var rel   = r.started_at ? relativeTime(r.started_at) : "?";
      html +=
        '<li class="recent-item" role="listitem">' +
        '<span class="recent-time" title="' + escHtml(r.started_at || "") + '">' + escHtml(rel) + '</span>' +
        '<span class="trigger-badge trigger-badge--small">' + escHtml(label) + '</span>' +
        '<span class="recent-detail">' + (r.events_armed != null ? r.events_armed + " armed" : "") +
        (r.elapsed_ms != null ? ", " + r.elapsed_ms + " ms" : "") + '</span>' +
        '</li>';
    }
    recentList.innerHTML = html;
    // Show "Show all 50" only if there's more than 10 and not already showing all
    recentShowAllWrap.hidden = (recentData.length <= 10 || recentShowAll);
  }

  function showStatusError(msg) {
    statusError.textContent = msg;
    statusError.hidden = false;
    statusGrid.hidden = true;
    recentActivity.hidden = true;
  }

  // -----------------------------------------------------------------------
  // M7: Status polling
  // -----------------------------------------------------------------------

  function fetchStatus() {
    fetch("status", { credentials: "same-origin" })
      .then(function (resp) {
        if (!resp.ok) throw new Error("HTTP " + resp.status);
        return resp.json();
      })
      .then(function (data) {
        renderStatus(data);
        // Sync debug toggle if the section is open
        if (hdrDebug.getAttribute("aria-expanded") !== "false") {
          if (typeof data.debug_logging === "boolean") {
            chkDebug.checked = data.debug_logging;
          }
        }
      })
      .catch(function () {
        showStatusError("Failed to load status");
      });
  }

  function startPolling() {
    if (pollTimer) clearInterval(pollTimer);
    pollTimer = setInterval(function () {
      if (!document.hidden) fetchStatus();
    }, pollInterval * 1000);
  }

  function resetPollTimer() {
    startPolling();
  }

  // Pause on hidden, resume + immediate fetch on visible
  document.addEventListener("visibilitychange", function () {
    if (!document.hidden) {
      fetchStatus();
    }
  });

  // Status panel collapse/expand
  hdrStatus.addEventListener("click", function () {
    var expanded = hdrStatus.getAttribute("aria-expanded") !== "false";
    expanded = !expanded;
    hdrStatus.setAttribute("aria-expanded", expanded ? "true" : "false");
    bodyStatus.hidden = !expanded;
  });

  // Recent activity expand/collapse
  btnRecentToggle.addEventListener("click", function () {
    var expanded = btnRecentToggle.getAttribute("aria-expanded") !== "false";
    expanded = !expanded;
    btnRecentToggle.setAttribute("aria-expanded", expanded ? "true" : "false");
    recentList.hidden = !expanded;
  });

  // Show all 50
  btnShowAllRecent.addEventListener("click", function () {
    recentShowAll = true;
    renderRecentList();
  });

  // -----------------------------------------------------------------------
  // M7: Recompute now
  // -----------------------------------------------------------------------

  btnRecompute.addEventListener("click", function () {
    btnRecompute.disabled = true;
    showStatus("Recomputing…");

    fetch("recompute", {
      method: "POST",
      credentials: "same-origin",
      headers: { "Content-Type": "application/json" },
      body: "{}"
    })
      .then(function (resp) {
        if (resp.status === 202) {
          return resp.json().then(function () {
            showStatus("Queued — recompute already in progress.", "");
          });
        }
        if (!resp.ok) {
          return resp.json().then(function (body) {
            throw new Error(body.message || ("HTTP " + resp.status));
          }, function () {
            throw new Error("HTTP " + resp.status);
          });
        }
        return resp.json().then(function () {
          showStatus("Recomputed.", "success");
          // Refresh status panel immediately to show new last_recompute
          fetchStatus();
        });
      })
      .catch(function (err) {
        showStatus("Failed — retry? (" + (err.message || "unknown error") + ")", "error");
      })
      .then(function () {
        btnRecompute.disabled = false;
      });
  });

  // -----------------------------------------------------------------------
  // M7: Import modal
  // -----------------------------------------------------------------------

  // All focusable elements inside the modal (for focus trap)
  function getFocusables() {
    return Array.prototype.slice.call(
      importModalDialog.querySelectorAll(
        'button, input, select, textarea, a[href], [tabindex]:not([tabindex="-1"])'
      )
    ).filter(function (el) { return !el.disabled; });
  }

  function openImportModal() {
    _importOpener = document.activeElement;
    importFile.value = "";
    importStatus.textContent = "";
    importStatus.className = "status";
    importModal.hidden = false;
    importModal.removeAttribute("aria-hidden");
    // Inert the rest of the page content
    document.querySelector("main").setAttribute("aria-hidden", "true");
    importModalDialog.focus();
  }

  function closeImportModal() {
    importModal.hidden = true;
    importModal.setAttribute("aria-hidden", "true");
    document.querySelector("main").removeAttribute("aria-hidden");
    if (_importOpener) {
      _importOpener.focus();
      _importOpener = null;
    }
  }

  btnImport.addEventListener("click", openImportModal);
  btnImportCancel.addEventListener("click", closeImportModal);

  // Close on Esc
  importModal.addEventListener("keydown", function (ev) {
    if (ev.key === "Escape") {
      closeImportModal();
      return;
    }
    // Focus trap: Tab / Shift-Tab cycles within modal
    if (ev.key === "Tab") {
      var focusables = getFocusables();
      if (focusables.length === 0) { ev.preventDefault(); return; }
      var first = focusables[0];
      var last  = focusables[focusables.length - 1];
      if (ev.shiftKey) {
        if (document.activeElement === first) {
          ev.preventDefault();
          last.focus();
        }
      } else {
        if (document.activeElement === last) {
          ev.preventDefault();
          first.focus();
        }
      }
    }
  });

  // Close on outside-click (click on backdrop, not dialog)
  importModal.addEventListener("click", function (ev) {
    if (ev.target === importModal) closeImportModal();
  });

  // Import submit
  btnImportSubmit.addEventListener("click", function () {
    var file = importFile.files && importFile.files[0];
    if (!file) {
      importStatus.textContent = "Select a file first.";
      importStatus.className = "status error";
      return;
    }

    btnImportSubmit.disabled = true;
    importStatus.textContent = "Importing…";
    importStatus.className = "status";

    var reader = new FileReader();
    reader.onload = function (ev2) {
      var text = ev2.target.result;
      fetch("import", {
        method: "POST",
        credentials: "same-origin",
        headers: { "Content-Type": "application/json" },
        body: text
      })
        .then(function (resp) {
          return resp.json().then(function (body) {
            if (!resp.ok) {
              var errMsg = body.message
                ? body.message + (body.error ? " (" + body.error + ")" : "")
                : (body.error || ("HTTP " + resp.status));
              throw new Error(errMsg);
            }
            return body;
          });
        })
        .then(function (body) {
          var imp = body.imported || {};
          var msg = "Imported " +
            (imp.anchors != null ? imp.anchors : "?") + " anchor" + (imp.anchors !== 1 ? "s" : "") + " / " +
            (imp.calendar != null ? imp.calendar : "?") + " calendar entr" + (imp.calendar !== 1 ? "ies" : "y");
          importStatus.textContent = msg;
          importStatus.className = "status success";
          btnImportSubmit.disabled = false;

          // Refresh schedule list and status panel
          loadSchedules();
          fetchStatus();

          // Auto-close after brief success display
          setTimeout(function () {
            if (!importModal.hidden) closeImportModal();
          }, 1800);
        })
        .catch(function (err) {
          importStatus.textContent = "Import failed: " + (err.message || "unknown error");
          importStatus.className = "status error";
          btnImportSubmit.disabled = false;
        });
    };
    reader.onerror = function () {
      importStatus.textContent = "Could not read file.";
      importStatus.className = "status error";
      btnImportSubmit.disabled = false;
    };
    reader.readAsText(file);
  });

  // -----------------------------------------------------------------------
  // M7: Debug-logging toggle
  // -----------------------------------------------------------------------

  // Debug section collapse/expand
  hdrDebug.addEventListener("click", function () {
    var expanded = hdrDebug.getAttribute("aria-expanded") !== "false";
    expanded = !expanded;
    hdrDebug.setAttribute("aria-expanded", expanded ? "true" : "false");
    bodyDebug.hidden = !expanded;
    if (expanded) {
      loadDebugState();
    }
  });

  function loadDebugState() {
    fetch("debug", { credentials: "same-origin" })
      .then(function (resp) {
        if (!resp.ok) throw new Error("HTTP " + resp.status);
        return resp.json();
      })
      .then(function (data) {
        chkDebug.checked = !!data.debug_logging;
        chkDebug.disabled = false;
      })
      .catch(function () {
        // Leave checkbox at its current state; don't crash
        chkDebug.disabled = true;
        debugIndicator.textContent = "Unavailable";
        debugIndicator.className = "toggle-indicator toggle-failed";
      });
  }

  chkDebug.addEventListener("change", function () {
    var newVal = chkDebug.checked;
    chkDebug.disabled = true;
    debugIndicator.textContent = "Saving…";
    debugIndicator.className = "toggle-indicator toggle-saving";

    fetch("debug", {
      method: "POST",
      credentials: "same-origin",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ debug_logging: newVal })
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
        chkDebug.checked = !!data.debug_logging;
        chkDebug.disabled = false;
        debugIndicator.textContent = "";
        debugIndicator.className = "toggle-indicator";
      })
      .catch(function (err) {
        // Revert
        chkDebug.checked = !newVal;
        chkDebug.disabled = false;
        debugIndicator.textContent = "Failed — retry?";
        debugIndicator.className = "toggle-indicator toggle-failed";
        debugIndicator.title = err.message || "";
        debugIndicator.onclick = function () {
          debugIndicator.onclick = null;
          chkDebug.click();
        };
      });
  });

  // -----------------------------------------------------------------------
  // M6: Render schedule sections
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
        // Visually-hidden "(built-in)" suffix for screen readers on lock-glyph rows
        var builtinNote = isBuiltin ? '<span class="sr-only">(built-in)</span>' : "";
        var toggleHtml =
          '<button type="button" class="toggle-btn" role="switch"' +
          ' aria-checked="' + checkedAttr + '"' +
          ' aria-label="' + escHtml(row.name) + (row.enabled ? " (enabled)" : " (disabled)") + '"' +
          ' data-id="' + escHtml(row.id) + '">' +
          "</button>";

        var nameHtml =
          '<div class="row-name">' +
          '<span class="row-name-text">' + escHtml(row.name) + "</span>" +
          (isBuiltin ? LOCK_SVG + builtinNote : "") +
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
    var toggleBtns = document.querySelectorAll(".toggle-btn");
    for (var i = 0; i < toggleBtns.length; i++) {
      toggleBtns[i].addEventListener("click", onToggleClick);
    }

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

    var rowData = null;
    for (var i = 0; i < rows.length; i++) {
      if (rows[i].id === id) { rowData = rows[i]; break; }
    }
    if (!rowData) return;

    // Optimistic UI update
    btn.setAttribute("aria-checked", newValue ? "true" : "false");
    btn.setAttribute("aria-label", rowData.name + (newValue ? " (enabled)" : " (disabled)"));
    rowData.enabled = newValue;

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
        if (newValue) {
          loadSchedules();
        } else {
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
        btn.setAttribute("aria-checked", current ? "true" : "false");
        btn.setAttribute("aria-label", rowData.name + (current ? " (enabled)" : " (disabled)"));
        rowData.enabled = current;
        btn.disabled = false;

        indicator.className = "toggle-indicator toggle-failed";
        indicator.textContent = "Failed — retry?";
        indicator.title = err.message || "Unknown error";

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
          rows = rows.filter(function (r) { return r.id !== id; });
          render();
        })
        .catch(function (err) {
          showStatus("Delete failed: " + err.message, "error");
        });
    });
  }

  function fetchReferenceCount(id, category, cb) {
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
          if (a.id === id) continue;
          if (a.event_source === id) count++;
          if (a.start_event === id) count++;
          if (a.end_event === id) count++;
        }
        cb(count);
      })
      .catch(function () { cb(0); });
  }

  // -----------------------------------------------------------------------
  // Section collapse/expand (M6 schedule sections)
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

  // -----------------------------------------------------------------------
  // Boot
  // -----------------------------------------------------------------------
  loadSchedules();
  fetchStatus();
  startPolling();
})();
