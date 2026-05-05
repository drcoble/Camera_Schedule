// Vanilla JS — no frameworks, no CDN (DL-08). Fetches the `about`
// FastCGI endpoint and renders {name, version, arch} into the page.

(function () {
  "use strict";

  function setText(id, value) {
    var el = document.getElementById(id);
    if (el) el.textContent = value;
  }

  function showError(message) {
    var el = document.getElementById("about-error");
    if (!el) return;
    el.textContent = message;
    el.hidden = false;
  }

  // Endpoint name comes from the manifest's httpConfig entry. The
  // camera resolves /local/<appName>/about → our FastCGI handler.
  fetch("about", { credentials: "same-origin" })
    .then(function (resp) {
      if (!resp.ok) throw new Error("HTTP " + resp.status);
      return resp.json();
    })
    .then(function (data) {
      setText("about-name", data.name || "—");
      setText("about-version", data.version || "—");
      setText("about-arch", data.arch || "—");
    })
    .catch(function (err) {
      showError("Could not load build info: " + err.message);
    });
})();
