// The default home page. One HTML file, one style sheet and one script, no
// external resources, no inline script or style: the lobby serves them with a
// Content-Security-Policy that allows nothing else. Everything a visitor or a
// room owner typed is put in the page with textContent, never as markup.

#include "npk/lobby/site.hpp"

namespace npk::lobby {

namespace {

const Asset kIndex{"text/html; charset=utf-8", R"npk(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="light dark">
<title>Game lobby</title>
<link rel="stylesheet" href="/style.css">
</head>
<body>
<header>
  <div class="wrap">
    <h1 id="title">Game lobby</h1>
    <p id="tagline" class="tagline"></p>
    <p id="description"></p>
    <p id="download-row" class="download" hidden>
      <a id="download" class="button" rel="noopener noreferrer"></a>
      <span id="download-note" class="note"></span>
    </p>
  </div>
</header>
<main class="wrap">
  <section aria-labelledby="join-h">
    <h2 id="join-h">How to join</h2>
    <ol id="steps"></ol>
    <p class="note">Lobby address for the game: <code id="address"></code></p>
  </section>
  <section aria-labelledby="rooms-h">
    <h2 id="rooms-h">Rooms right now</h2>
    <p id="status" role="status" class="note">Loading…</p>
    <ul id="rooms" class="rooms"></ul>
  </section>
  <noscript><p class="note">This page needs JavaScript to list the rooms.</p></noscript>
</main>
<footer class="wrap note">Powered by netplay-kit.</footer>
<script src="/app.js"></script>
</body>
</html>
)npk"};

const Asset kStyle{"text/css; charset=utf-8", R"npk(:root {
  color-scheme: light dark;
  --bg: #f6f7f9; --panel: #ffffff; --text: #1b1f24; --muted: #5b6573; --line: #dde1e7;
  --accent: #2563eb; --accent-text: #ffffff; --open: #166534; --open-bg: #dcfce7; --busy: #854d0e; --busy-bg: #fef3c7;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #0f1216; --panel: #181c22; --text: #e8ebef; --muted: #99a3b0; --line: #2a3039;
    --accent: #60a5fa; --accent-text: #0b1220; --open: #86efac; --open-bg: #14351f; --busy: #fde68a; --busy-bg: #3a2f0b;
  }
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--text); font: 16px/1.5 system-ui, -apple-system, "Segoe UI", sans-serif; }
.wrap { max-width: 44rem; margin: 0 auto; padding: 0 16px; }
header { background: var(--panel); border-bottom: 1px solid var(--line); padding: 28px 0 24px; }
h1 { margin: 0 0 4px; font-size: 2rem; line-height: 1.2; }
h2 { margin: 28px 0 8px; font-size: 1.15rem; }
.tagline { margin: 0 0 12px; color: var(--muted); font-size: 1.1rem; }
.note { color: var(--muted); font-size: 0.9rem; }
.download { display: flex; flex-wrap: wrap; align-items: center; gap: 12px; margin: 16px 0 0; }
.download[hidden] { display: none; }
.button { display: inline-block; padding: 10px 18px; border-radius: 8px; background: var(--accent); color: var(--accent-text);
  font-weight: 600; text-decoration: none; }
.button:hover, .button:focus-visible { filter: brightness(1.1); }
.button:focus-visible { outline: 3px solid var(--text); outline-offset: 2px; }
code { padding: 1px 6px; border-radius: 4px; background: var(--panel); border: 1px solid var(--line); user-select: all; }
ol { padding-left: 1.3rem; }
li { margin: 4px 0; }
.rooms { list-style: none; margin: 0; padding: 0; display: grid; gap: 8px; }
.room { display: flex; flex-wrap: wrap; align-items: center; justify-content: space-between; gap: 8px 16px;
  padding: 12px 14px; background: var(--panel); border: 1px solid var(--line); border-radius: 10px; }
.room .name { font-weight: 600; overflow-wrap: anywhere; }
.room .meta { display: flex; align-items: center; gap: 10px; color: var(--muted); font-size: 0.9rem; }
.badge { padding: 2px 10px; border-radius: 999px; font-size: 0.8rem; font-weight: 600; background: var(--open-bg); color: var(--open); }
.badge.playing { background: var(--busy-bg); color: var(--busy); }
main { padding-bottom: 24px; }
footer { padding: 8px 16px 40px; }
)npk"};

const Asset kScript{"text/javascript; charset=utf-8", R"npk("use strict";
(function () {
  var REFRESH_MS = 5000;

  function byId(id) { return document.getElementById(id); }
  function setText(el, s) { el.textContent = s == null ? "" : String(s); }

  function getJson(path) {
    return fetch(path, { headers: { accept: "application/json" }, cache: "no-store" }).then(function (r) {
      if (!r.ok) { throw new Error("HTTP " + r.status); }
      return r.json();
    });
  }

  function isWebUrl(u) { return typeof u === "string" && /^https?:\/\//i.test(u); }

  function renderSite(site) {
    if (site.title) { setText(byId("title"), site.title); document.title = site.title; }
    setText(byId("tagline"), site.tagline);
    setText(byId("description"), site.description);
    var d = site.download;
    if (d && isWebUrl(d.url)) {
      var a = byId("download");
      a.href = d.url;
      setText(a, d.label || "Download");
      setText(byId("download-note"), d.note);
      byId("download-row").hidden = false;
    }
    var steps = byId("steps");
    steps.textContent = "";
    (site.steps || []).forEach(function (s) {
      var li = document.createElement("li");
      setText(li, s);
      steps.appendChild(li);
    });
  }

  function renderRooms(rooms) {
    var list = byId("rooms");
    list.textContent = "";
    rooms.forEach(function (r) {
      if (r.state !== "open" && r.state !== "playing") { return; }
      var li = document.createElement("li");
      li.className = "room";
      var name = document.createElement("span");
      name.className = "name";
      setText(name, r.name);
      var meta = document.createElement("span");
      meta.className = "meta";
      var seats = document.createElement("span");
      setText(seats, r.taken + " of " + r.seat_count + " seats");
      var badge = document.createElement("span");
      badge.className = "badge" + (r.state === "playing" ? " playing" : "");
      setText(badge, r.state === "playing" ? "In a match" : "Open");
      meta.appendChild(seats);
      meta.appendChild(badge);
      li.appendChild(name);
      li.appendChild(meta);
      list.appendChild(li);
    });
    return list.children.length;
  }

  function refresh() {
    Promise.all([getJson("/api/rooms"), getJson("/api/status")]).then(function (res) {
      var shown = renderRooms(res[0].rooms || []);
      var servers = res[1].game_servers || 0;
      var msg = shown === 0 ? "No rooms yet. Create one from the game." : "";
      if (servers === 0) { msg = (msg ? msg + " " : "") + "No game server is online, so matches cannot start right now."; }
      setText(byId("status"), msg);
    }).catch(function () {
      setText(byId("status"), "Cannot reach the lobby. Trying again…");
    }).then(function () {
      if (!document.hidden) { setTimeout(refresh, REFRESH_MS); } else { waiting = true; }
    });
  }

  var waiting = false;
  document.addEventListener("visibilitychange", function () {
    if (!document.hidden && waiting) { waiting = false; refresh(); }
  });

  setText(byId("address"), window.location.origin);
  getJson("/api/site").then(renderSite).catch(function () {});
  refresh();
})();
)npk"};

}  // namespace

const Asset* default_asset(std::string_view path) {
  if (path == "/" || path == "/index.html") return &kIndex;
  if (path == "/style.css") return &kStyle;
  if (path == "/app.js") return &kScript;
  return nullptr;
}

}  // namespace npk::lobby
