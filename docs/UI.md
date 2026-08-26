# BLLED v3 web UI — developer notes

The whole interface is three files in `src/www/`, plus a standalone captive-portal page.
No framework, no build step, no CDN: `pre_build.py` gzips every asset in `src/www/` into
`src/www/www.h` and the firmware serves the compressed bytes straight from PROGMEM.

## File layout

| File | Role |
|---|---|
| `src/www/index.html` | SPA shell: header, restart banner, tab/bottom nav, the hand-written **Dashboard** markup, the static **System** cards, the toast/tooltip/modal hosts, and an inline SVG icon sprite. The four configuration sections are empty `<section>` elements filled by `app.js`. |
| `src/www/app.js` | Everything else. Reading order: `TIPS` → option tables → `SECTIONS` schema → field renderers → dirty/save logic → WiFi/printer pickers → nav → live layer (`/ws` + polling) → dashboard renderer → animation loop → System wiring → `boot()`. |
| `src/www/style.css` | Design tokens on `:root`, a `prefers-color-scheme: light` override, then components. Shared by `index.html`; a trimmed copy is inlined in `wifiSetup.html`. |
| `src/www/wifiSetup.html` | Captive-portal page served at `/wifi`. Fully self-contained (CSS + JS inlined) because Android/iOS captive-portal mini-browsers do not reliably load sub-resources. |
| `src/www/webSerialPage.html` | Belongs to the WebSerial library — do not edit. |
| `src/www/blled.svg`, `favicon.png` | Logo and tab icon. |

Removed in v3: `setupPage.html`, `updatePage.html`, `backupRestore.html` (folded into the SPA)
and `particleCanvas.js` (unused by the new design).

## Running the mock server

```
python3 tools/mock_server.py                    # http://localhost:8080, serves src/www
python3 tools/mock_server.py --port 9000 -v     # another port, log every request
python3 tools/mock_server.py --cycle 600 --hms  # slow print cycle, always raise an HMS
python3 tools/mock_server.py --offset 560       # start partway into the cycle (mid-print)
```

Standard library only (`http.server` + a ~60-line RFC6455 WebSocket). It implements every
endpoint in `ARCHITECTURE.md` §7, including the legacy aliases, multipart OTA and restore
uploads, and `/ws` (1 Hz push plus an immediate push after every mutation, and the
`{"cmd":"led"}` / `{"cmd":"clearLed"}` client commands).

The simulated X1C walks `idle → preparing → preheating → lidar stages (14, 1, 8, 9) →
homing → first-layer inspection → printing → finish → idle` once per `--cycle` seconds
(default 120), advancing temperatures, fans, layers and progress, opening the door after a
finish, and raising an HMS message on roughly a third of the cycles. `PUT /api/config`
validates key names, enum values, `#rrggbb` colours and HMS code syntax and clamps numeric
ranges exactly like `validateConfig()` is meant to, so a UI bug that sends a wrong key gets
a 400 here instead of quietly working. Configuration lives in memory; `POST /api/config/reset`
restores the defaults.

`--hms`, `--offset` and `-v` are mock-only conveniences and have no firmware equivalent.

## Layer progress on the dashboard

The printer card draws print and layer progress together and updates both only when a status
frame arrives — no canvas and no per-frame JS. The existing SVG ring (`#d-ring`, r=42) keeps
`printer.progress` in the accent colour; a second, thinner concentric ring inside it
(`#d-ring2`, r=32, circumference 201, `var(--info)`) shows `layer / totalLayers`, and a
`.rleg` legend plus a `title` on `.ringcol` name the two. `renderDash()` writes one
`stroke-dasharray` per ring and sets a single custom property `--p` (0–100) on the gauge
container `#d-lg`; CSS derives everything else from it — the fill is
`height: calc(var(--p) * 1%)` and the knob is
`transform: translateY(calc(var(--p) * var(--lh) / -100))`, both with a `.45s` transition, so
the animation costs the compositor and not the main thread. The vertical gauge (`.lg`) runs
from the `#i-print` sprite glyph at the bottom (first layer) to a small top marker, with the
layer percentage on the knob and `layer / total layers` in the caption `#d-layer` — that
caption replaces the old *Layer* row in the details list. When `totalLayers` is 0 (idle, or a
printer that never reports it) the inner ring and the gauge sit at 0 and the caption reads
*no layer data*; the knob keeps a `var(--surf)` background with `var(--info)` text so it stays
legible in both themes.

## How tooltips map to config keys

`TIPS` in `app.js` is one flat object keyed by the **`PrinterConfig` field name** from
`src/blled/types.h` (plus `wifiSSID`, `wifiPass`, `host`, `webUser`, `webPass` and three
UI-only keys: `override`, `ota`, `backup`):

```js
TIPS.followChamberLight = "Mirrors the printer's own chamber light: …";
```

A field in the `SECTIONS` schema carries `k` (its tooltip key). `tipBtn(k)` renders the `?`
button only when `TIPS[k]` exists, so an unwritten tooltip degrades to no button rather than
an empty popover. The popover opens on hover, on click/tap and on keyboard focus, and closes
on Escape, scroll, resize or an outside click.

Colour fields are the one place where the tooltip key and the payload keys differ. A field
`{k:'runningColor', base:'running', t:'color'}` looks its tooltip up under `runningColor`
(the `types.h` field name) and reads/writes the three flat JSON keys `runningRGB`,
`runningWW`, `runningCW`. `fieldKeys()` performs that expansion and `sectionKeys()` collects
the whole set a tab owns — which is exactly the body of that tab's `PUT /api/config`.

`docs/manual.md` reuses the same texts verbatim; if you change a tooltip, update the manual
entry with it.

## Saving

Each configuration tab has its own save bar. Saving sends **only that tab's keys** as a
partial `PUT /api/config`; the response (the full config) is adopted as the new baseline.
Secrets (`wifiPass`, `webPass`, `mqttExtPass`) are rendered empty with a `(unchanged)`
placeholder and are only included in the body once the user has typed in them — the server
also treats `"********"` as "unchanged". Any key in `NETKEYS`, or a response carrying
`restartRequired: true`, raises the restart banner; the device never restarts on its own.

The unsaved-changes count is a plain diff of `draft` against `cfg`, shown in the save bar and
as a dot on the nav item, and it arms a `beforeunload` prompt.

## Live data

`connectWs()` opens `/ws` and falls back to polling `/api/status` every 2 s if the socket
never opens or drops; the header dot shows `live` / `polling` / `reconnecting` / `no data`.
Every status frame re-renders the dashboard.

A single `requestAnimationFrame` loop drives all animation. It composites the five channels
into the colour the strip actually emits (`composite()`, warm-white and cold-white tints
added to RGB and clamped) and applies the same effect modulation curves as `leds.h` §4.2 —
`breathe` 6 s → 1.5 s, `blink` 1.2 s → 0.3 s, `fastblink` 0.3 s → 0.1 s, `rainbow` 60 s → 6 s
across `effectSpeed` 1–10 — so the on-screen preview matches the hardware. The same loop
animates the little preview chips beside the effect and visualisation selectors.

## Size budget

Total gzipped `src/www/` must stay under 60 kB (see `ARCHITECTURE.md` §3).

```
$ for f in src/www/*.html src/www/*.js src/www/*.css src/www/*.svg src/www/*.png; do
    printf '%-28s %6s\n' "$f" "$(gzip -9 -c "$f" | wc -c)"; done
```

At the time of writing: `app.js` 21.3 kB, `style.css` 4.4 kB, `index.html` 3.8 kB,
`wifiSetup.html` 3.7 kB, `webSerialPage.html` 3.3 kB, `favicon.png` 1.0 kB, `blled.svg` 0.9 kB
— **38.5 kB total**, about 21 kB of headroom.

## Screenshots

`docs/screenshots/` holds one PNG per section at 1280 px (desktop tabs) and 375 px (mobile
bottom nav), captured against the mock:

* `dashboard`, `led`, `events`, `alerts`, `connection`, `system` × `-1280.png` / `-375.png`
* `dashboard-light-1280.png` — the same dashboard under `prefers-color-scheme: light`
* `wifisetup-375.png` — the captive-portal page

They were taken by driving headless Chrome over the DevTools protocol against
`--cycle 900 --hms --offset 560` (a mid-print printer with an active HMS message); there is
no screenshot script checked in, so regenerate them by hand after a visual change.

## Browser support / accessibility notes

ES2018, no optional chaining, no `??`. Every interactive element is a real `<button>`,
`<input>` or `<select>`; the tab bar and bottom nav are one `role="tablist"` pair over the
same `role="tabpanel"` sections; focus rings are never removed. The layout is verified free of
horizontal overflow at 360 px.
