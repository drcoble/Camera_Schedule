// Vanilla JS — no frameworks, no CDN (DL-08). Fetches the `about`
// FastCGI endpoint and renders {name, version, arch} into the page.

(function () {
  "use strict";

  function setText(id, value) {
    var el = document.getElementById(id);
    if (el) el.textContent = value;
  }

  // Render a URL as a safe external link. Only http(s) URLs become
  // anchors; anything else falls back to plain text.
  function setLink(id, url) {
    var el = document.getElementById(id);
    if (!el) return;
    el.textContent = "";
    if (typeof url === "string" && /^https?:\/\//.test(url)) {
      var a = document.createElement("a");
      a.href = url;
      a.textContent = url;
      a.rel = "noopener noreferrer";
      a.target = "_blank";
      el.appendChild(a);
    } else {
      el.textContent = url || "—";
    }
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
      setLink("about-repo", data.repo);
    })
    .catch(function (err) {
      showError("Could not load build info: " + err.message);
    });
})();
