// Vanilla JS — no frameworks, no CDN (DL-08).
// GET  /local/camera_schedule/location  → {lat, lon}
// POST /local/camera_schedule/location  body {lat, lon}  → updated {lat, lon}

(function () {
  "use strict";

  var form     = document.getElementById("location-form");
  var latInput = document.getElementById("lat");
  var lonInput = document.getElementById("lon");
  var status   = document.getElementById("status");
  var reload   = document.getElementById("reload");

  function show(message, kind) {
    status.textContent = message;
    status.className = "status" + (kind ? " " + kind : "");
  }

  function loadFromCamera() {
    show("Loading current location…");
    fetch("location", { credentials: "same-origin" })
      .then(function (resp) {
        if (!resp.ok) throw new Error("HTTP " + resp.status);
        return resp.json();
      })
      .then(function (data) {
        latInput.value = data.lat;
        lonInput.value = data.lon;
        show("Loaded from camera.");
      })
      .catch(function (err) {
        show("Could not load location: " + err.message, "error");
      });
  }

  form.addEventListener("submit", function (ev) {
    ev.preventDefault();
    var lat = parseFloat(latInput.value);
    var lon = parseFloat(lonInput.value);
    if (!isFinite(lat) || !isFinite(lon)) {
      show("Latitude and longitude must be valid numbers.", "error");
      return;
    }
    show("Saving…");
    fetch("location", {
      method: "POST",
      credentials: "same-origin",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ lat: lat, lon: lon })
    })
      .then(function (resp) {
        if (!resp.ok) {
          return resp.text().then(function (txt) {
            throw new Error("HTTP " + resp.status + (txt ? ": " + txt : ""));
          });
        }
        return resp.json();
      })
      .then(function (data) {
        latInput.value = data.lat;
        lonInput.value = data.lon;
        show("Saved. Sunrise / sunset will recompute now.", "success");
      })
      .catch(function (err) {
        show("Save failed: " + err.message, "error");
      });
  });

  reload.addEventListener("click", loadFromCamera);

  loadFromCamera();
})();
