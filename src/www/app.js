/* BLLED v3 single-page web UI.
   Vanilla ES2018, no framework, no external requests. Talks to /api/* and /ws only.
   Layout: TIPS (one tooltip per config key) -> SECTIONS (form schema) -> renderers -> live layer.
   See docs/UI.md. */
(function () {
"use strict";

/* ------------------------------------------------------------------ utils */
var $ = function (s, r) { return (r || document).querySelector(s); };
var $$ = function (s, r) { return Array.prototype.slice.call((r || document).querySelectorAll(s)); };
function el(tag, attrs, kids) {
  var n = document.createElement(tag);
  if (attrs) for (var k in attrs) {
    if (k === "class") n.className = attrs[k];
    else if (k === "text") n.textContent = attrs[k];
    else if (k === "html") n.innerHTML = attrs[k];
    else if (attrs[k] !== null && attrs[k] !== undefined && attrs[k] !== false) n.setAttribute(k, attrs[k]);
  }
  (kids || []).forEach(function (c) { if (c) n.appendChild(c); });
  return n;
}
function svgIcon(id) {
  var s = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  var u = document.createElementNS("http://www.w3.org/2000/svg", "use");
  u.setAttribute("href", "#" + id); s.appendChild(u); s.setAttribute("aria-hidden", "true");
  return s;
}
function clamp(v, a, b) { return v < a ? a : v > b ? b : v; }
function num(v, d) { v = parseFloat(v); return isFinite(v) ? v : d; }
function fmtDur(s) {
  s = Math.max(0, Math.round(s));
  if (s < 60) return s + "s";
  var m = Math.floor(s / 60), h = Math.floor(m / 60);
  if (h) return h + "h " + (m % 60) + "m";
  return m + "m " + (s % 60) + "s";
}
function fmtT(v) { return (v === null || v === undefined || isNaN(v)) ? "—" : Math.round(v) + "°"; }
function esc(s) { return String(s === null || s === undefined ? "" : s); }

/* ------------------------------------------------------------- tooltips */
/* One entry per config key (plus a few UI-only keys). Reused verbatim by docs/manual.md. */
var TIPS = {
  /* --- LED general --- */
  ledMode: "Picks what the strip does. Auto follows the printer (the whole point of BLLED); Maintenance and Test hold a fixed colour for working on the machine or checking your wiring; Rainbow is decorative and looks good in timelapses; WiFi strength colours the strip by signal so you can find a good spot for the controller; Off kills the output without unplugging anything.",
  brightness: "Global output level, applied last to every channel. 0 % switches all five channels fully off, 100 % is full PWM duty. Cheap 12 V strips get noticeably warm above ~70 % — if the LEDs flicker or the printer's chamber camera blooms, turn this down before changing colours.",
  fadeMs: "How long the strip takes to cross-fade when the colour changes, in milliseconds. 500 ms feels smooth and hides the constant micro-changes during a print; 0 makes every state change a hard cut, which is what you want if you are filming the LEDs or debugging the state machine.",
  effectSpeed: "Speed of the breathe / blink / rainbow animations, 1 (slow) to 10 (fast). At 5 a breathe cycle takes about 3 seconds and a blink about 0.75 s. Only affects animated effects — it does nothing while everything is set to Solid.",
  followChamberLight: "Mirrors the printer's own chamber light: when you switch the light off in the Bambu app or on the screen, BLLED goes dark too, and comes back when you switch it on. Leave this on if you want one light switch for the whole printer; turn it off if BLLED is your only chamber light and should stay on regardless.",
  runningColor: "The everyday colour — shown while printing, preheating, homing and while the printer sits idle. Warm white plus cold white at full is the neutral default; add RGB only if you want a tint. This is the colour you will be looking at 95 % of the time, so set it first.",
  maintenanceColor: "Colour used while LED mode is Maintenance. Defaults to both whites at maximum for the brightest, most neutral working light, which is what you want when you are clearing a clog or re-seating a hotend.",
  testColor: "Colour used while LED mode is Test. A saturated colour (default #3F3CFB) makes it obvious which of the five channels are actually wired: if you see white instead of blue your warm/cold white lines are swapped in.",
  wifiColor: "Shown during boot while the controller is joining WiFi, and on the setup access point. Default orange. If the strip stays this colour, BLLED never finished connecting — check the Connection tab.",
  printingVisual: "How the running colour behaves during an actual print. Solid never changes; Progress blends the running colour towards the finish colour as the print advances, so a glance at the strip tells you roughly how far along it is; Breathe pulses gently so you can see at a distance that the machine is still working.",
  preheatVisual: "How the strip looks while the bed or hotend is coming up to temperature. Solid shows the plain running colour; Temp glow ramps the brightness with the temperature and adds a red tint below 30 %, so the strip visibly 'warms up' with the printer.",

  /* --- print events --- */
  finishIndication: "Switches the strip to the finish colour when a print completes, so you can see from across the room that the plate is ready. Turn off if you would rather the LEDs just go back to the normal running colour.",
  finishColor: "Colour shown after a successful print. Default green. Something clearly different from your running colour works best — the whole point is to be noticeable from the doorway.",
  finishEffect: "Animation for the finish colour. Solid is calm; Breathe draws the eye without being annoying; Blink is hard to miss if the printer is out of sight. Blinking for a long finish timeout will irritate everyone in the room.",
  finishExitMode: "How the finish colour ends. Door waits until you open or close the printer door — it stays lit until you actually come and collect the print. Timer clears it after a fixed number of minutes. P1 printers have no door sensor, so use Timer there.",
  finishTimerMins: "Minutes the finish colour stays on before the strip returns to normal, when exit mode is Timer. Ignored in Door mode.",
  inactivityEnabled: "Turns the LEDs off after the printer has been idle for a while, so the strip is not burning all night. Any activity from the printer — a new print, a door event, a temperature change — brings the light straight back.",
  inactivityMins: "Minutes of printer inactivity before the LEDs switch off. The timer restarts on any printer report change and on every door open/close, so it only fires when the machine is genuinely untouched.",
  controlChamberLight: "Lets BLLED drive the printer's own chamber light over MQTT: on when a print starts or the door opens, off when the inactivity timeout fires or the door gesture switches the LEDs off. Handy when BLLED and the chamber light should behave as one lamp; leave off if you control the chamber light from Home Assistant or the app.",
  doorToggleEnabled: "Closing the door twice within two seconds toggles the LEDs on or off — a physical light switch that needs no phone. Useful during a timelapse or when a bright chamber annoys you at night. P1 printers have no door sensor, so this never triggers there.",
  offlineTimeoutSec: "How long the strip keeps its last colour after the printer's MQTT connection drops, before going dark. A few seconds of grace avoids flicker on brief WiFi hiccups; longer values keep the light on through a printer reboot.",
  isP1Printer: "Tells BLLED you have a P1-series printer. P1 machines have no Micro Lidar and no door sensor, so the lidar stage colours and the door gesture do nothing; switching this on sets the lidar stages to plain white so those stages simply stay lit instead of going dark.",
  lidarStagesEnabled: "During bed levelling, nozzle cleaning, extrusion calibration, bed scanning and first-layer inspection the X1's Micro Lidar takes measurements, and bright external light can disturb it. When enabled, the stage colours below are used instead of the running colour (default: off/black) so the strip gets out of the way. P1 printers have no lidar — leave this off.",
  stage14Color: "Colour while the printer is cleaning the nozzle tip (stage 14). Default off, so the lidar sees a dark chamber.",
  stage1Color: "Colour during auto bed levelling (stage 1). Default off — this is the longest lidar stage and the one most affected by stray light.",
  stage8Color: "Colour while calibrating extrusion / flow (stage 8). Default off.",
  stage9Color: "Colour while scanning the bed surface (stage 9). Default off.",
  stage10Color: "Colour while inspecting the first layer (stage 10). Default off. This stage also fires mid-print, so if you dislike the light dropping out during a print, set a colour here.",

  /* --- errors --- */
  errorDetection: "Master switch for every alert colour on this tab. When off, HMS messages, pauses and error stages are ignored and the strip just keeps showing the normal running colour. Turn it off if you find the red interruptions more annoying than useful.",
  errorEffect: "Animation used for all error colours (HMS, filament runout, front cover, temperature faults). Blink is the loudest and is genuinely useful for a fatal error you must notice.",
  pauseEffect: "Animation used for the pause colours (user pause, G-code pause, first-layer error, nozzle clog). Breathe reads as 'waiting for you' without shouting.",
  pauseColor: "Shown when the print is paused by you or by an M400/G-code pause. Default blue — deliberately not red, because a pause is not a fault.",
  firstLayerColor: "Shown when the printer pauses because first-layer inspection failed (stage 34). Default blue. Give it its own colour if you want to tell 'come and look at the plate' apart from an ordinary pause.",
  nozzleClogColor: "Shown when the printer reports a nozzle clog pause (stage 35). Default blue.",
  hmsSeriousColor: "Shown when the most severe active HMS message is Serious — something needs attention but the printer usually keeps going. Default red.",
  hmsFatalColor: "Shown when the most severe active HMS message is Fatal — the printer has stopped and needs you. Default red; pair it with a blinking error effect if the machine is out of earshot.",
  hmsCommonEnabled: "Also react to Common (advisory) HMS messages, such as an AMS humidity warning. Off by default because these are frequent and mostly harmless; enable it with a distinct colour if you want to see advisories without confusing them with real faults.",
  hmsCommonColor: "Colour for Common (advisory) HMS messages when they are enabled. Default orange — pick something that is clearly not your fatal/serious red.",
  filamentRunoutColor: "Shown when the printer pauses because filament ran out (stage 6 / the matching HMS code). Default red.",
  frontCoverColor: "Shown when the printer reports the front cover falling off or missing (stage 17). Default red.",
  nozzleTempColor: "Shown on a nozzle temperature malfunction pause (stage 20). Default red — this is a genuine hardware fault, not a hint.",
  bedTempColor: "Shown on a heat-bed temperature malfunction pause (stage 21). Default red.",
  hmsIgnoreList: "HMS codes that should never change the LED colour, one per line, in the form HMS_0300_1200_0002_0001. Use it for the nuisance code your printer reports constantly (a known AMS quirk, a sensor you have already decided to live with) so it stops turning the strip red. Add codes straight from the Dashboard with the '+ ignore' button.",

  /* --- connection --- */
  wifiSSID: "Name of the 2.4 GHz WiFi network the controller joins. The ESP32 has no 5 GHz radio, so if your router hides the 2.4 GHz band behind one combined SSID the join can fail — give the 2.4 GHz band its own name. Changing this needs a restart.",
  wifiPass: "Password for that network. It is stored in plain text in the config file on the device (and in backups), so treat a backup like a password. Leave blank to keep the existing one.",
  BSSID: "Pins the controller to one specific access point by MAC address instead of letting it roam. Useful in a mesh where the ESP32 keeps clinging to a distant node; leave empty unless you have that problem.",
  rescanWiFiNetwork: "On the next connect, scan and join the strongest access point for this SSID instead of the pinned BSSID. A one-shot request — it is not stored.",
  host: "Controller name. It is the mDNS hostname (http://<name>.local), the DHCP name your router shows, and the default external-MQTT topic prefix. Letters, digits and hyphens only; changing it needs a restart and re-publishes Home Assistant discovery.",
  printerIP: "The Bambu printer's IP address on your LAN. Give the printer a DHCP reservation or a static lease — if it moves, BLLED loses MQTT until you update this (or until auto-update finds it again).",
  printerAutoIp: "Keep the printer IP up to date automatically: when network discovery sees your serial number at a new address, BLLED follows it. Leave on unless you have two printers and want to be certain BLLED never re-points itself.",
  serialNumber: "The printer's serial number, printed on the machine and shown under Settings on the printer's screen. It is the MQTT topic and it also tells BLLED your model (X1C, P1S, A1…). It must match exactly or no reports will arrive.",
  accessCode: "The eight-character LAN access code from the printer's network settings screen. It is the MQTT password. Regenerating it on the printer, or a firmware update, invalidates the old one — re-enter it here if the printer stops reporting.",
  webUser: "Optional user name for HTTP Basic authentication on this web interface. Leave both user and password empty to keep the UI open on your LAN. Once set, it protects every route including the API, firmware upload and backup download.",
  webPass: "Password for the web interface login. Leave blank to keep the current one; clear the user name to disable authentication entirely. If you lock yourself out, a factory reset (or the USB serial provisioning) is the way back in.",
  mqttExtEnabled: "Publishes everything BLLED knows to your own MQTT broker, and accepts commands back. This is how you get the controller into Home Assistant, Node-RED or any other automation. It is a second, plain (non-TLS) connection and is completely separate from the printer's own MQTT link.",
  mqttExtHost: "Hostname or IP of your MQTT broker, e.g. the machine running Mosquitto. Plain TCP only — TLS brokers are not supported.",
  mqttExtPort: "Broker port. 1883 is the standard unencrypted MQTT port.",
  mqttExtUser: "Broker user name. Leave empty for an anonymous broker.",
  mqttExtPass: "Broker password. Leave blank to keep the stored one.",
  mqttExtBaseTopic: "Prefix for every topic BLLED publishes, e.g. blled/livingroom gives blled/livingroom/status and blled/livingroom/set. Leave empty to use blled/<controller name>. Change it if you run more than one BLLED on the same broker.",
  mqttExtIntervalSec: "How often the full status object is republished even when nothing changed, in seconds. Changes are always published within a second regardless; this is the heartbeat. Raise it if you are logging every message to disk.",
  haDiscovery: "Publishes Home Assistant MQTT discovery messages so the controller appears as a device with a light, sensors and buttons without any YAML. Switching it off removes those entities from Home Assistant again.",
  haPrefix: "Discovery topic prefix Home Assistant listens on. 'homeassistant' unless you deliberately changed it in your Home Assistant MQTT settings.",

  /* --- debug --- */
  debugVerbose: "Logs everything to the serial console and the web serial log. Very chatty — the printer sends a report every second — so use it while chasing a problem and turn it back off afterwards.",
  debugChanges: "Logs only when something actually changes: stage transitions, LED decisions, door events, connection changes. This is the useful one to leave on; it is quiet when the printer is quiet.",
  debugMqtt: "Logs the filtered contents of every printer MQTT report. Goes to the USB serial port only (never the web log) because it is far too much traffic for a WebSocket. For diagnosing parsing problems.",

  /* --- UI-only --- */
  override: "Force any colour onto the strip right now, ignoring the printer state — useful for testing a colour before committing to it, or for using BLLED as a plain lamp for a while. Set minutes to have it release itself automatically, or 0 to hold it until you press Clear.",
  ota: "Uploads a new firmware image over WiFi. Use the .bin built for this board; the wrong image will not boot and needs a USB cable to recover. Settings survive the update.",
  backup: "Download the complete configuration as a JSON file, or upload one you saved earlier. Restoring replaces every setting (it is not a merge) and restarts the controller."
};

/* -------------------------------------------------------------- options */
var MODES = [
  ["auto", "Auto", "Follow the printer — the normal mode."],
  ["maintenance", "Maintenance", "Always on in the maintenance colour, whatever the printer does."],
  ["test", "Test", "Always on in the test colour, for checking wiring and channels."],
  ["rainbow", "Rainbow", "Decorative colour cycle; ignores printer state."],
  ["wifi", "WiFi", "Colour shows the controller's WiFi signal strength."],
  ["off", "Off", "LED output disabled."]
];
var EFFECTS = [["solid", "Solid"], ["breathe", "Breathe"], ["blink", "Blink"], ["fastblink", "Fast blink"]];
var PVIS = [["solid", "Solid"], ["progress", "Progress blend"], ["breathe", "Breathe"]];
var HVIS = [["solid", "Solid"], ["tempglow", "Temp glow"]];
var EXITS = [["door", "Door open/close"], ["timer", "After a timer"]];

/* --------------------------------------------------------------- schema */
function C(k, base, l, d) { return { k: k, base: base, l: l, t: "color", d: d }; }

var SECTIONS = [
  { id: "dash", label: "Dashboard", short: "Home", icon: "i-dash" },

  { id: "led", label: "LED Behaviour", short: "LEDs", icon: "i-led", groups: [
    { title: "Mode", fields: [
      { k: "ledMode", l: "LED mode", t: "seg", opts: MODES }
    ] },
    { title: "Output", fields: [
      { k: "brightness", l: "Brightness", t: "range", min: 0, max: 100, unit: "%" },
      { k: "fadeMs", l: "Fade time", t: "range", min: 0, max: 5000, step: 50, unit: " ms" },
      { k: "effectSpeed", l: "Effect speed", t: "range", min: 1, max: 10 },
      { k: "followChamberLight", l: "Follow the printer's chamber light", t: "bool" }
    ] },
    { title: "Colours", fields: [
      C("runningColor", "running", "Running / idle colour"),
      C("maintenanceColor", "maintenance", "Maintenance colour"),
      C("testColor", "test", "Test colour"),
      C("wifiColor", "wifi", "Boot / WiFi colour")
    ] },
    { title: "Visualisations", desc: "Extra life for the running colour while the printer works.", fields: [
      { k: "printingVisual", l: "While printing", t: "select", opts: PVIS, fx: "printing" },
      { k: "preheatVisual", l: "While preheating", t: "select", opts: HVIS, fx: "preheat" }
    ] }
  ] },

  { id: "events", label: "Print Events", short: "Print", icon: "i-print", groups: [
    { title: "Finish indication", fields: [
      { k: "finishIndication", l: "Signal a finished print", t: "bool" },
      C("finishColor", "finish", "Finish colour"),
      { k: "finishEffect", l: "Finish effect", t: "effect" },
      { k: "finishExitMode", l: "Leave the finish colour", t: "select", opts: EXITS },
      { k: "finishTimerMins", l: "After", t: "int", min: 0, max: 999, unit: " min", when: function (d) { return d.finishExitMode === "timer"; } }
    ], when: function (d) { return d.finishIndication; }, always: ["finishIndication"] },
    { title: "Idle & door", fields: [
      { k: "inactivityEnabled", l: "Switch off when idle", t: "bool" },
      { k: "inactivityMins", l: "After", t: "int", min: 0, max: 999, unit: " min", when: function (d) { return d.inactivityEnabled; } },
      { k: "controlChamberLight", l: "Also control the printer's chamber light", t: "bool" },
      { k: "doorToggleEnabled", l: "Door double-close toggles the LEDs", t: "bool" },
      { k: "offlineTimeoutSec", l: "Go dark when the printer is offline for", t: "int", min: 0, max: 999, unit: " s" }
    ] },
    { title: "Printer type", fields: [
      { k: "isP1Printer", l: "P1-series printer (no lidar, no door sensor)", t: "bool" }
    ] },
    { title: "Lidar stages", desc: "Stages where the Micro Lidar measures and stray light hurts.", fields: [
      { k: "lidarStagesEnabled", l: "Use dedicated colours for lidar stages", t: "bool" },
      C("stage14Color", "stage14", "14 — Cleaning nozzle tip"),
      C("stage1Color", "stage1", "1 — Auto bed levelling"),
      C("stage8Color", "stage8", "8 — Calibrating extrusion"),
      C("stage9Color", "stage9", "9 — Scanning bed surface"),
      C("stage10Color", "stage10", "10 — Inspecting first layer")
    ], when: function (d) { return d.lidarStagesEnabled; }, always: ["lidarStagesEnabled"] }
  ] },

  { id: "alerts", label: "Errors & Alerts", short: "Alerts", icon: "i-alert", groups: [
    { title: "Detection", fields: [
      { k: "errorDetection", l: "React to errors and HMS messages", t: "bool" },
      { k: "errorEffect", l: "Error effect", t: "effect" },
      { k: "pauseEffect", l: "Pause effect", t: "effect" }
    ], when: function (d) { return d.errorDetection; }, always: ["errorDetection"] },
    { title: "Pause colours", fields: [
      C("pauseColor", "pause", "Paused by user or G-code"),
      C("firstLayerColor", "firstLayer", "First-layer inspection failed"),
      C("nozzleClogColor", "nozzleClog", "Nozzle clog")
    ], when: function (d) { return d.errorDetection; } },
    { title: "Fault colours", fields: [
      C("hmsFatalColor", "hmsFatal", "HMS — Fatal"),
      C("hmsSeriousColor", "hmsSerious", "HMS — Serious"),
      { k: "hmsCommonEnabled", l: "Also react to Common advisories", t: "bool" },
      (function () { var f = C("hmsCommonColor", "hmsCommon", "HMS — Common");
        f.when = function (d) { return d.hmsCommonEnabled; }; return f; })(),
      C("filamentRunoutColor", "filamentRunout", "Filament runout"),
      C("frontCoverColor", "frontCover", "Front cover falling"),
      C("nozzleTempColor", "nozzleTemp", "Nozzle temperature fault"),
      C("bedTempColor", "bedTemp", "Bed temperature fault")
    ], when: function (d) { return d.errorDetection; } },
    { title: "Ignored HMS codes", fields: [
      { k: "hmsIgnoreList", l: "Ignore list", t: "hms" }
    ] }
  ] },

  { id: "conn", label: "Connection", short: "Network", icon: "i-net", groups: [
    { title: "WiFi", net: true, fields: [
      { k: "wifiSSID", l: "Network (SSID)", t: "text", max: 32, scan: true },
      { k: "wifiPass", l: "Password", t: "secret" },
      { k: "BSSID", l: "Pin to access point (BSSID)", t: "text", max: 17, ph: "aa:bb:cc:dd:ee:ff" },
      { k: "rescanWiFiNetwork", l: "Re-scan for the strongest AP on next connect", t: "bool" },
      { k: "host", l: "Controller name", t: "text", max: 32 }
    ] },
    { title: "Printer", net: true, fields: [
      { k: "printerIP", l: "Printer IP address", t: "text", max: 15, ph: "192.168.1.100", discover: true },
      { k: "printerAutoIp", l: "Follow the printer if its IP changes", t: "bool" },
      { k: "serialNumber", l: "Serial number", t: "text", max: 15, ph: "00M09A1234567890" },
      { k: "accessCode", l: "LAN access code", t: "pass", max: 8 }
    ] },
    { title: "Web interface", net: true, fields: [
      { k: "webUser", l: "User name", t: "text", max: 39 },
      { k: "webPass", l: "Password", t: "secret" }
    ] },
    { title: "External MQTT / Home Assistant", live: "mqtt", fields: [
      { k: "mqttExtEnabled", l: "Publish to my own MQTT broker", t: "bool" },
      { k: "mqttExtHost", l: "Broker host", t: "text", max: 63, ph: "192.168.1.10" },
      { k: "mqttExtPort", l: "Port", t: "int", min: 1, max: 65535 },
      { k: "mqttExtUser", l: "User name", t: "text", max: 31 },
      { k: "mqttExtPass", l: "Password", t: "secret" },
      { k: "mqttExtBaseTopic", l: "Base topic", t: "text", max: 47, ph: "blled/<controller name>" },
      { k: "mqttExtIntervalSec", l: "Publish interval", t: "int", min: 1, max: 999, unit: " s" },
      { k: "haDiscovery", l: "Home Assistant auto-discovery", t: "bool" },
      { k: "haPrefix", l: "Discovery prefix", t: "text", max: 23 }
    ], when: function (d) { return d.mqttExtEnabled; }, always: ["mqttExtEnabled"] }
  ] },

  { id: "sys", label: "System", short: "System", icon: "i-sys", debug: [
    { k: "debugChanges", l: "Log state changes", t: "bool" },
    { k: "debugVerbose", l: "Verbose log", t: "bool" },
    { k: "debugMqtt", l: "Log printer MQTT reports (USB serial only)", t: "bool" }
  ] }
];

/* Keys a section owns (used for the partial PUT and the dirty check). */
function fieldKeys(f) {
  return f.t === "color" ? [f.base + "RGB", f.base + "WW", f.base + "CW"] : [f.k];
}
function sectionKeys(sec) {
  var out = [];
  (sec.groups || []).forEach(function (g) { g.fields.forEach(function (f) { out = out.concat(fieldKeys(f)); }); });
  (sec.debug || []).forEach(function (f) { out = out.concat(fieldKeys(f)); });
  return out;
}
var SECRETS = { wifiPass: 1, webPass: 1, mqttExtPass: 1 };
var NETKEYS = { wifiSSID: 1, wifiPass: 1, BSSID: 1, host: 1, printerIP: 1, serialNumber: 1, accessCode: 1, webUser: 1, webPass: 1 };

/* ---------------------------------------------------------------- state */
var cfg = {};          /* last config from the server */
var draft = {};        /* local edits */
var status = null;     /* last /api/status */
var info = null;       /* /api/info */
var cur = "dash";
var busy = {};
var restartNeeded = false;

/* ------------------------------------------------------------------ api */
function api(path, opts) {
  opts = opts || {};
  return fetch(path, opts).then(function (r) {
    var ct = r.headers.get("content-type") || "";
    var p = ct.indexOf("json") >= 0 ? r.json() : r.text();
    return p.then(function (body) {
      if (!r.ok) throw new Error((body && body.error) || ("HTTP " + r.status));
      return body;
    });
  });
}
function put(obj) {
  return api("/api/config", {
    method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify(obj)
  });
}
function post(path, obj) {
  return api(path, {
    method: "POST", headers: { "Content-Type": "application/json" },
    body: obj === undefined ? "{}" : JSON.stringify(obj)
  });
}

/* ---------------------------------------------------------------- toast */
function toast(msg, kind) {
  var t = el("div", { class: "toast " + (kind || ""), text: msg });
  $("#toasts").appendChild(t);
  setTimeout(function () { t.remove(); }, kind === "err" ? 6000 : 3200);
}
function confirmBox(title, body, okLabel) {
  return new Promise(function (res) {
    var m = $("#modal");
    $("#m-title").textContent = title; $("#m-body").textContent = body;
    $("#m-yes").textContent = okLabel || "Confirm";
    m.hidden = false; $("#m-yes").focus();
    function done(v) { m.hidden = true; $("#m-yes").onclick = null; $("#m-no").onclick = null; res(v); }
    $("#m-yes").onclick = function () { done(true); };
    $("#m-no").onclick = function () { done(false); };
  });
}

/* -------------------------------------------------------------- tooltip */
var tipEl, tipOwner = null;
function showTip(btn) {
  var txt = TIPS[btn.dataset.k];
  if (!txt) return;
  tipEl.textContent = txt;
  tipEl.hidden = false;
  var r = btn.getBoundingClientRect();
  var w = tipEl.offsetWidth;
  var left = clamp(r.left + window.pageXOffset + r.width / 2 - w / 2, 8, window.innerWidth - w - 8);
  var top = r.bottom + window.pageYOffset + 7;
  if (r.bottom + tipEl.offsetHeight + 20 > window.innerHeight && r.top > tipEl.offsetHeight + 20) {
    top = r.top + window.pageYOffset - tipEl.offsetHeight - 7;
  }
  tipEl.style.left = left + "px"; tipEl.style.top = top + "px";
  btn.setAttribute("aria-expanded", "true");
  tipOwner = btn;
}
function hideTip() {
  tipEl.hidden = true;
  if (tipOwner) tipOwner.setAttribute("aria-expanded", "false");
  tipOwner = null;
}
function wireTips() {
  tipEl = $("#tip");
  document.addEventListener("pointerover", function (e) {
    var b = e.target.closest && e.target.closest(".tip");
    if (b && b !== tipOwner) showTip(b);
  });
  document.addEventListener("pointerout", function (e) {
    var b = e.target.closest && e.target.closest(".tip");
    if (b && b === tipOwner && document.activeElement !== b) hideTip();
  });
  document.addEventListener("click", function (e) {
    var b = e.target.closest && e.target.closest(".tip");
    if (b) { e.preventDefault(); if (b === tipOwner && !tipEl.hidden) hideTip(); else showTip(b); }
    else if (!e.target.closest || !e.target.closest("#tip")) hideTip();
  });
  document.addEventListener("focusin", function (e) {
    var b = e.target.closest && e.target.closest(".tip");
    if (b) showTip(b); else if (tipOwner) hideTip();
  });
  document.addEventListener("keydown", function (e) { if (e.key === "Escape") { hideTip(); $("#modal").hidden = true; } });
  window.addEventListener("resize", hideTip);
  window.addEventListener("scroll", hideTip, true);
}
function tipBtn(k) {
  return TIPS[k] ? el("button", { class: "tip", type: "button", "data-k": k, "aria-expanded": "false", "aria-label": "Help", text: "?" }) : null;
}

/* --------------------------------------------------------- colour maths */
/* Composite the five channels into the colour the strip actually emits. */
var WW_T = [1.00, 0.72, 0.42], CW_T = [0.80, 0.90, 1.00];
function composite(r, g, b, ww, cw) {
  var w = ww / 255, c = cw / 255;
  return [
    clamp(r + 255 * w * WW_T[0] + 255 * c * CW_T[0], 0, 255),
    clamp(g + 255 * w * WW_T[1] + 255 * c * CW_T[1], 0, 255),
    clamp(b + 255 * w * WW_T[2] + 255 * c * CW_T[2], 0, 255)
  ];
}
function rgbCss(a, m) {
  m = m === undefined ? 1 : m;
  return "rgb(" + Math.round(a[0] * m) + "," + Math.round(a[1] * m) + "," + Math.round(a[2] * m) + ")";
}
function hex2rgb(h) {
  h = String(h || "").replace("#", "");
  if (h.length === 3) h = h[0] + h[0] + h[1] + h[1] + h[2] + h[2];
  if (!/^[0-9a-fA-F]{6}$/.test(h)) return [0, 0, 0];
  return [parseInt(h.slice(0, 2), 16), parseInt(h.slice(2, 4), 16), parseInt(h.slice(4, 6), 16)];
}
function rgb2hex(a) {
  return "#" + a.map(function (v) { return ("0" + clamp(Math.round(v), 0, 255).toString(16)).slice(-2); }).join("");
}
function hsl2rgb(h) { /* full saturation/lightness 0.5 rainbow */
  var f = function (n) {
    var k = (n + h / 30) % 12;
    return 255 * (0.5 - 0.5 * Math.max(-1, Math.min(k - 3, 9 - k, 1)));
  };
  return [f(0), f(8), f(4)];
}
/* Effect modulation, mirroring leds.h §4.2 so the preview matches the hardware. */
function period(effect, speed) {
  var t = (clamp(speed, 1, 10) - 1) / 9;
  if (effect === "breathe") return 6000 + (1500 - 6000) * t;
  if (effect === "blink") return 1200 + (300 - 1200) * t;
  if (effect === "fastblink") return 300 + (100 - 300) * t;
  if (effect === "rainbow") return 60000 + (6000 - 60000) * t;
  return 1000;
}
function modulation(effect, speed, now) {
  if (!effect || effect === "solid") return 1;
  var p = period(effect, speed), ph = (now % p) / p;
  if (effect === "breathe") return 0.25 + 0.75 * (0.5 + 0.5 * Math.sin(ph * 2 * Math.PI - Math.PI / 2));
  if (effect === "blink" || effect === "fastblink") return ph < 0.5 ? 1 : 0;
  return 1;
}

/* --------------------------------------------------------- field render */
function labelRow(f, control, inline) {
  var lab = el("label", { text: f.l, for: "f-" + f.k });
  var left = el("div", { class: "left" }, [lab, tipBtn(f.k)]);
  var lbl = el("div", { class: "lbl" }, inline ? [left, control] : [lab, tipBtn(f.k)]);
  var wrap = el("div", { class: "f" + (inline ? " inline" : ""), "data-fk": f.k }, inline ? [lbl] : [lbl, control]);
  if (f.d) wrap.appendChild(el("p", { class: "desc", text: f.d }));
  return wrap;
}
function get(k) { return draft[k]; }
function set(k, v) { draft[k] = v; refreshDirty(); applyConditions(); }

function renderField(f, secId) {
  var t = f.t, id = "f-" + f.k, node;

  if (t === "bool") {
    var cb = el("input", { type: "checkbox", id: id });
    cb.checked = !!get(f.k);
    cb.onchange = function () { set(f.k, cb.checked); };
    return labelRow(f, el("span", { class: "sw" }, [cb, el("i")]), true);
  }

  if (t === "range") {
    var r = el("input", { type: "range", id: id, min: f.min, max: f.max, step: f.step || 1 });
    var o = el("output");
    r.value = num(get(f.k), f.min);
    o.textContent = r.value + (f.unit || "");
    r.oninput = function () { o.textContent = r.value + (f.unit || ""); set(f.k, parseInt(r.value, 10)); };
    return labelRow(f, el("div", { class: "rng" }, [r, o]));
  }

  if (t === "int") {
    var n = el("input", { type: "number", id: id, min: f.min, max: f.max, style: "width:110px" });
    n.value = num(get(f.k), f.min);
    n.oninput = function () { set(f.k, clamp(parseInt(n.value, 10) || 0, f.min, f.max)); };
    var box = el("div", { class: "row" }, [n]);
    if (f.unit) box.appendChild(el("span", { class: "small muted", text: f.unit.trim() }));
    return labelRow(f, box, true);
  }

  if (t === "text" || t === "pass" || t === "secret") {
    var inp = el("input", {
      type: t === "text" ? "text" : "password", id: id,
      maxlength: f.max, placeholder: t === "secret" ? "(unchanged)" : (f.ph || ""),
      autocomplete: "off", autocapitalize: "off", spellcheck: "false"
    });
    inp.value = t === "secret" ? "" : esc(get(f.k));
    if (t === "secret") inp.dataset.touched = "";
    inp.oninput = function () {
      if (t === "secret") { inp.dataset.touched = "1"; draft[f.k] = inp.value; refreshDirty(); }
      else set(f.k, inp.value);
    };
    var row = el("div", { class: "row" }, [inp]);
    inp.style.flex = "1 1 180px";
    if (t === "pass" || t === "secret") {
      var eye = el("button", { class: "btn sm", type: "button", text: "Show" });
      eye.onclick = function () {
        var p = inp.type === "password";
        inp.type = p ? "text" : "password"; eye.textContent = p ? "Hide" : "Show";
      };
      row.appendChild(eye);
    }
    if (f.scan) {
      var sc = el("button", { class: "btn sm", type: "button", text: "Scan" });
      sc.onclick = function () { wifiScan(inp, sc); };
      row.appendChild(sc);
      row.appendChild(el("div", { class: "pick", id: "wifilist", hidden: "hidden", style: "flex-basis:100%" }));
    }
    if (f.discover) {
      var dc = el("button", { class: "btn sm", type: "button", text: "Discover" });
      dc.onclick = function () { discover(inp, dc); };
      row.appendChild(dc);
      row.appendChild(el("div", { class: "pick", id: "printerlist", hidden: "hidden", style: "flex-basis:100%" }));
    }
    var w = labelRow(f, row);
    if (t === "secret") w.appendChild(el("p", { class: "desc", text: "Leave blank to keep the stored password." }));
    return w;
  }

  if (t === "select" || t === "effect") {
    var opts = t === "effect" ? EFFECTS : f.opts;
    var s = el("select", { id: id, style: "width:auto;min-width:160px" });
    opts.forEach(function (o) { s.appendChild(el("option", { value: o[0], text: o[1] })); });
    s.value = esc(get(f.k));
    /* an animated preview chip, but only for selectors that actually change the light */
    var kind = t === "effect" ? "effect" : f.fx;
    var prev = kind ? el("span", { class: "fx", "data-fx": kind, title: "preview" }) : null;
    if (prev) prev.dataset.val = s.value;
    s.onchange = function () { if (prev) prev.dataset.val = s.value; set(f.k, s.value); };
    return labelRow(f, el("div", { class: "row" }, prev ? [s, prev] : [s]), true);
  }

  if (t === "seg") {
    var seg = el("div", { class: "seg", role: "group" });
    var desc = el("p", { class: "desc" });
    f.opts.forEach(function (o) {
      var b = el("button", { type: "button", text: o[1], "aria-pressed": String(get(f.k) === o[0]) });
      b.onclick = function () {
        $$("button", seg).forEach(function (x) { x.setAttribute("aria-pressed", "false"); });
        b.setAttribute("aria-pressed", "true"); desc.textContent = o[2] || "";
        set(f.k, o[0]);
      };
      if (get(f.k) === o[0]) desc.textContent = o[2] || "";
      seg.appendChild(b);
    });
    var wrap = el("div", { class: "f", "data-fk": f.k }, [
      el("div", { class: "lbl" }, [el("label", { text: f.l }), tipBtn(f.k)]), seg, desc
    ]);
    return wrap;
  }

  if (t === "color") return colorField(f);

  if (t === "hms") {
    var ta = el("textarea", { id: id, spellcheck: "false", placeholder: "HMS_0300_1200_0002_0001" });
    ta.value = String(get(f.k) || "").split(",").filter(Boolean).join("\n");
    var err = el("p", { class: "desc" });
    ta.oninput = function () {
      var lines = ta.value.split(/[\n,;\s]+/).map(function (s) { return s.trim().toUpperCase().replace(/-/g, "_"); }).filter(Boolean);
      var bad = lines.filter(function (s) { return !/^HMS(_[0-9A-F]{4}){4}$/.test(s); });
      err.textContent = bad.length ? "Not a valid HMS code: " + bad.join(", ") : (lines.length + " code" + (lines.length === 1 ? "" : "s") + " ignored");
      err.style.color = bad.length ? "var(--err)" : "";
      set(f.k, lines.join(","));
    };
    ta.oninput();
    return labelRow(f, el("div", {}, [ta, err]));
  }

  return el("div");
}

/* One reusable colour control: RGB picker + WW/CW ranges + composited swatch. */
function colorField(f) {
  var kR = f.base + "RGB", kW = f.base + "WW", kC = f.base + "CW";
  var sw = el("span");
  var swatch = el("div", { class: "sw2", title: "Actual strip colour" }, [sw]);
  var pick = el("input", { type: "color", id: "f-" + f.k, "aria-label": f.l + " RGB" });
  var wwR = el("input", { type: "range", min: 0, max: 255, "aria-label": "Warm white" });
  var cwR = el("input", { type: "range", min: 0, max: 255, "aria-label": "Cold white" });
  var wwO = el("output"), cwO = el("output");

  function paint() {
    var c = composite.apply(null, hex2rgb(get(kR)).concat([num(get(kW), 0), num(get(kC), 0)]));
    sw.style.background = rgbCss(c);
    wwO.textContent = num(get(kW), 0); cwO.textContent = num(get(kC), 0);
  }
  function sync() { pick.value = /^#[0-9a-f]{6}$/i.test(get(kR) || "") ? get(kR) : "#000000"; wwR.value = num(get(kW), 0); cwR.value = num(get(kC), 0); paint(); }
  pick.oninput = function () { set(kR, pick.value.toLowerCase()); paint(); };
  wwR.oninput = function () { set(kW, parseInt(wwR.value, 10)); paint(); };
  cwR.oninput = function () { set(kC, parseInt(cwR.value, 10)); paint(); };

  var off = el("button", { class: "btn sm", type: "button", text: "Off" });
  off.onclick = function () { draft[kR] = "#000000"; draft[kW] = 0; draft[kC] = 0; sync(); refreshDirty(); };
  var white = el("button", { class: "btn sm", type: "button", text: "White" });
  white.onclick = function () { draft[kR] = "#000000"; draft[kW] = 255; draft[kC] = 255; sync(); refreshDirty(); };

  sync();
  var body = el("div", { class: "cf" }, [
    swatch, pick,
    el("div", { class: "wcs" }, [
      el("label", { text: "WW" }), wwR, wwO,
      el("label", { text: "CW" }), cwR, cwO
    ]),
    off, white
  ]);
  var w = labelRow(f, body);
  w.dataset.sync = "1";
  w._sync = sync;
  return w;
}

/* --------------------------------------------------------- section render */
function renderSection(sec) {
  var host = $("#s-" + sec.id);
  if (sec.id === "sys") {
    var dbg = $("#y-debug"); dbg.innerHTML = "";
    sec.debug.forEach(function (f) { dbg.appendChild(renderField(f, sec.id)); });
    return;
  }
  host.innerHTML = "";
  sec.groups.forEach(function (g) {
    var card = el("div", { class: "card", "data-grp": g.title });
    var head = el("header", {}, [el("h2", { text: g.title })]);
    if (g.live === "mqtt") head.appendChild(el("span", { class: "sub", id: "conn-mqttstate" }));
    card.appendChild(head);
    if (g.desc) card.appendChild(el("p", { class: "desc", style: "margin-top:-6px", text: g.desc }));
    g.fields.forEach(function (f) { card.appendChild(renderField(f, sec.id)); });
    if (g.net) card.appendChild(el("p", { class: "desc", text: "Changing these needs a controller restart." }));
    host.appendChild(card);
  });
  var bar = el("div", { class: "card" }, [el("div", { class: "savebar" }, [
    (function () {
      var b = el("button", { class: "btn pri", "data-save": sec.id, text: "Save " + sec.label });
      return b;
    })(),
    (function () {
      var b = el("button", { class: "btn", "data-revert": sec.id, text: "Revert" });
      return b;
    })(),
    el("span", { class: "st", "data-st": sec.id, text: "Saved" })
  ])]);
  host.appendChild(bar);
  applyConditions();
}

/* Show/hide dependent fields and whole groups. */
function applyConditions() {
  SECTIONS.forEach(function (sec) {
    (sec.groups || []).forEach(function (g) {
      var card = $('#s-' + sec.id + ' [data-grp="' + g.title + '"]');
      if (!card) return;
      var gOn = !g.when || g.when(draft);
      var anyVisible = false;
      g.fields.forEach(function (f) {
        var node = $('[data-fk="' + f.k + '"]', card);
        if (!node) return;
        var keep = g.always && g.always.indexOf(f.k) >= 0;
        var on = (gOn || keep) && (!f.when || f.when(draft));
        node.classList.toggle("hide", !on);
        if (on) anyVisible = true;
      });
      card.classList.toggle("hide", !anyVisible);
    });
  });
}

/* --------------------------------------------------------- dirty / save */
function dirtyKeys(sec) {
  var out = [];
  sectionKeys(sec).forEach(function (k) {
    if (SECRETS[k]) { var i = $("#f-" + k); if (i && i.dataset.touched === "1" && i.value !== "") out.push(k); return; }
    if (String(draft[k]) !== String(cfg[k])) out.push(k);
  });
  return out;
}
function refreshDirty() {
  SECTIONS.forEach(function (sec) {
    if (sec.id === "dash") return;
    var n = dirtyKeys(sec).length;
    var st = $('[data-st="' + sec.id + '"]');
    if (st) {
      st.textContent = n ? (n + " unsaved change" + (n === 1 ? "" : "s")) : "Saved";
      st.classList.toggle("dirty", !!n);
    }
    $$('[data-nav="' + sec.id + '"]').forEach(function (b) {
      var ind = $(".ind", b);
      if (n && !ind) b.appendChild(el("span", { class: "ind", title: "Unsaved changes" }));
      if (!n && ind) ind.remove();
    });
  });
}
function saveSection(secId) {
  var sec = SECTIONS.filter(function (s) { return s.id === secId; })[0];
  var keys = sectionKeys(sec), body = {}, net = false;
  keys.forEach(function (k) {
    if (SECRETS[k]) {
      var i = $("#f-" + k);
      if (i && i.dataset.touched === "1") { body[k] = i.value; if (NETKEYS[k]) net = true; }
      return;
    }
    body[k] = draft[k];
    if (NETKEYS[k] && String(draft[k]) !== String(cfg[k])) net = true;
  });
  var btn = $('[data-save="' + secId + '"]');
  if (btn) btn.disabled = true;
  return put(body).then(function (fresh) {
    adoptConfig(fresh);
    $$("[data-touched]").forEach(function (i) { i.dataset.touched = ""; i.value = ""; });
    toast(sec.label + " saved", "ok");
    if (fresh.restartRequired || net) showRestart();
    refreshDirty();
  }).catch(function (e) {
    toast("Save failed: " + e.message, "err");
  }).then(function () { if (btn) btn.disabled = false; });
}
function showRestart() { restartNeeded = true; $("#banner").hidden = false; }

function adoptConfig(c) {
  Object.keys(c).forEach(function (k) { if (k !== "restartRequired") cfg[k] = c[k]; });
  Object.keys(cfg).forEach(function (k) { draft[k] = cfg[k]; });
}
function rerenderAll() {
  SECTIONS.forEach(function (s) { if (s.groups || s.debug) renderSection(s); });
  refreshDirty();
}

/* ------------------------------------------------------------ wifi/scan */
function wifiScan(input, btn) {
  var list = $("#wifilist");
  list.hidden = false; list.innerHTML = "<div class='small muted' style='padding:8px 10px'>Scanning…</div>";
  btn.disabled = true;
  var tries = 0;
  (function poll() {
    api("/api/wifi/scan").then(function (r) {
      if (r.scanning && ++tries < 12) return setTimeout(poll, 1500);
      btn.disabled = false;
      var nets = (r.networks || []);
      if (!nets.length) { list.innerHTML = "<div class='small muted' style='padding:8px 10px'>No networks found.</div>"; return; }
      list.innerHTML = "";
      nets.forEach(function (n) {
        var b = el("button", { type: "button" }, [
          el("span", { text: (n.secure ? "🔒 " : "") + n.ssid }),
          el("span", { class: "rs", text: n.rssi + " dBm · ch " + (n.channel || "?") })
        ]);
        b.onclick = function () {
          input.value = n.ssid; set("wifiSSID", n.ssid);
          if (n.bssid) { var bi = $("#f-BSSID"); if (bi && bi.value) { bi.value = n.bssid; set("BSSID", n.bssid); } }
          list.hidden = true;
        };
        list.appendChild(b);
      });
    }).catch(function (e) { btn.disabled = false; list.innerHTML = "<div class='small' style='padding:8px 10px;color:var(--err)'>" + esc(e.message) + "</div>"; });
  })();
}
function discover(input, btn) {
  var list = $("#printerlist");
  list.hidden = false; list.innerHTML = "<div class='small muted' style='padding:8px 10px'>Searching…</div>";
  btn.disabled = true;
  post("/api/action", { action: "discover" }).catch(function () {}).then(function () {
    var tries = 0;
    (function poll() {
      api("/api/printers").then(function (r) {
        var ps = Array.isArray(r) ? r : (r.printers || []);
        if (!ps.length && ++tries < 8) return setTimeout(poll, 1500);
        btn.disabled = false;
        if (!ps.length) { list.innerHTML = "<div class='small muted' style='padding:8px 10px'>No printers found. Check that the printer is on the same subnet and not in cloud-only mode.</div>"; return; }
        list.innerHTML = "";
        ps.forEach(function (p) {
          var b = el("button", { type: "button" }, [
            el("span", { text: (p.model || "Bambu") + " · " + p.ip }),
            el("span", { class: "rs", text: p.usn || "" })
          ]);
          b.onclick = function () {
            input.value = p.ip; set("printerIP", p.ip);
            if (p.usn) { var si = $("#f-serialNumber"); if (si && !si.value) { si.value = p.usn; set("serialNumber", p.usn); } }
            list.hidden = true;
          };
          list.appendChild(b);
        });
      }).catch(function (e) { btn.disabled = false; list.innerHTML = "<div class='small' style='padding:8px 10px;color:var(--err)'>" + esc(e.message) + "</div>"; });
    })();
  });
}

/* --------------------------------------------------------------- navigation */
function buildNav() {
  var tabs = $("#tabs"), bnav = $("#bnav");
  SECTIONS.forEach(function (s) {
    [[tabs, s.label], [bnav, s.short]].forEach(function (pair) {
      var b = el("button", { type: "button", role: "tab", "data-nav": s.id, "aria-selected": String(s.id === cur) });
      b.appendChild(svgIcon(s.icon));
      b.appendChild(el("span", { text: pair[1] }));
      b.onclick = function () { go(s.id); };
      pair[0].appendChild(b);
    });
  });
}
function go(id) {
  cur = id;
  SECTIONS.forEach(function (s) { $("#s-" + s.id).hidden = s.id !== id; });
  $$("[data-nav]").forEach(function (b) { b.setAttribute("aria-selected", String(b.dataset.nav === id)); });
  if (location.hash.slice(1) !== id) history.replaceState(null, "", "#" + id);
  window.scrollTo(0, 0);
  hideTip();
}

/* ----------------------------------------------------------- live status */
var ws = null, wsOk = false, pollTimer = null, lastMsg = 0;
function connectWs() {
  try {
    ws = new WebSocket((location.protocol === "https:" ? "wss://" : "ws://") + location.host + "/ws");
  } catch (e) { startPolling(); return; }
  ws.onopen = function () { wsOk = true; setLive("ok", "live"); stopPolling(); };
  ws.onmessage = function (ev) {
    try { onStatus(JSON.parse(ev.data)); } catch (e) {}
  };
  ws.onclose = ws.onerror = function () {
    if (wsOk) setLive("warn", "reconnecting");
    wsOk = false; ws = null; startPolling();
    setTimeout(connectWs, 5000);
  };
}
function startPolling() {
  if (pollTimer) return;
  pollTimer = setInterval(function () {
    api("/api/status").then(onStatus).catch(function () { setLive("bad", "offline"); });
  }, 2000);
}
function stopPolling() { if (pollTimer) { clearInterval(pollTimer); pollTimer = null; } }
function setLive(cls, txt) {
  $("#livedot").className = "dot " + cls;
  $("#livetxt").textContent = txt;
}
function onStatus(s) {
  status = s; lastMsg = Date.now();
  setLive("ok", wsOk ? "live" : "polling");
  renderDash(s);
}
setInterval(function () {
  if (lastMsg && Date.now() - lastMsg > 8000) setLive("bad", "no data");
}, 3000);

/* ------------------------------------------------------------- dashboard */
var LEDCOUNT = 16;
var brightHold = 0;   /* millis of the last local brightness edit */
function renderDash(s) {
  var d = s.device || {}, p = s.printer || {}, l = s.led || {}, t = s.timers || {}, m = s.mqtt || {};

  $("#hostline").textContent = (d.host || "BLLED") + (d.ip ? " · " + d.ip : "");
  $("#d-fw").textContent = "v" + (d.fw || "?") + (d.apMode ? " · setup AP" : "");

  /* LED card */
  $("#d-reason").textContent = l.reason || "—";
  $("#d-outvals").textContent = "R" + l.r + " G" + l.g + " B" + l.b + " WW" + l.ww + " CW" + l.cw + " @" + l.brightness + "%";
  $("#d-ledsrc").className = "dot " + (l.override ? "warn" : (l.r || l.g || l.b || l.ww || l.cw) ? "ok" : "");
  $("#d-ledmodesub").textContent = (MODES.filter(function (o) { return o[0] === l.mode; })[0] || ["", l.mode])[1];
  $$("#d-mode button").forEach(function (b) { b.setAttribute("aria-pressed", String(b.dataset.v === l.mode)); });
  $("#d-modedesc").textContent = (MODES.filter(function (o) { return o[0] === l.mode; })[0] || ["", "", ""])[2];
  /* do not fight the user while they are dragging (touch drags may not set focus) */
  if (Date.now() - brightHold > 2000 && document.activeElement !== $("#d-bright")) {
    $("#d-bright").value = l.brightness; $("#d-brightv").textContent = l.brightness + " %";
  }
  $("#d-ovleft").textContent = l.override
    ? (l.overrideRemainingSec ? "Override active, " + fmtDur(l.overrideRemainingSec) + " left." : "Override active until cleared.")
    : "";

  /* printer */
  $("#d-model").textContent = p.model || "Printer";
  $("#d-pdot").className = "dot " + (p.connected ? "ok" : "bad");
  $("#d-pconn").textContent = p.connected ? ("report " + (p.lastReportSec || 0) + "s ago") : "disconnected";
  $("#d-gstate").textContent = p.gcodeState || "—";
  $("#d-stage").textContent = (p.stageName || "—") + (p.stage !== undefined ? " (" + p.stage + ")" : "");
  $("#d-layer").textContent = p.totalLayers ? p.layer + " / " + p.totalLayers : (p.layer || "—");
  $("#d-job").textContent = p.jobName || "—"; $("#d-job").title = p.jobName || "";
  $("#d-pfw").textContent = p.fw || "—";
  var pct = num(p.progress, 0);
  $("#d-pct").textContent = pct + "%";
  $("#d-eta").textContent = p.remainingMin ? fmtDur(p.remainingMin * 60) + " left" : "—";
  $("#d-ring").setAttribute("stroke-dasharray", (pct * 2.639).toFixed(1) + " 264");

  /* temps */
  var tw = $("#d-temps"); tw.innerHTML = "";
  [["Nozzle", p.nozzleTemp, p.nozzleTarget, 320], ["Bed", p.bedTemp, p.bedTarget, 120], ["Chamber", p.chamberTemp, null, 70]].forEach(function (g) {
    if (g[1] === null || g[1] === undefined) return;
    var frac = clamp(g[1] / g[3], 0, 1) * 100;
    var kids = [el("div", { class: "fill", style: "width:" + frac.toFixed(1) + "%" })];
    if (g[2]) kids.push(el("div", { class: "tgt", style: "left:" + clamp(g[2] / g[3], 0, 1) * 100 + "%", title: "target " + fmtT(g[2]) }));
    tw.appendChild(el("div", { class: "gauge" }, [
      el("div", { class: "top" }, [
        el("span", { text: g[0] }),
        el("b", { text: fmtT(g[1]) + (g[2] ? " / " + fmtT(g[2]) : "") })
      ]),
      el("div", { class: "trk" }, kids)
    ]));
  });

  /* fans */
  var fw = $("#d-fans"); fw.innerHTML = "";
  [["Part cooling", p.fanPart], ["Aux", p.fanAux], ["Chamber", p.fanChamber], ["Heatbreak", p.fanHeatbreak]].forEach(function (g) {
    fw.appendChild(el("div", { class: "b" }, [
      el("span", { text: g[0] }),
      el("div", { class: "trk" }, [el("div", { class: "fill", style: "width:" + clamp(num(g[1], 0), 0, 100) + "%" })]),
      el("span", { text: num(g[1], 0) + "%" })
    ]));
  });

  /* chips */
  var cw = $("#d-chips"); cw.innerHTML = "";
  function chip(txt, on, cls) { cw.appendChild(el("span", { class: "chip " + (on ? (cls || "on") : ""), text: txt })); }
  chip(p.doorOpen ? "Door open" : "Door closed", p.doorOpen, "hot");
  chip("Chamber light " + (p.chamberLight ? "on" : "off"), p.chamberLight);
  chip("Work light " + (p.workLight ? "on" : "off"), p.workLight);
  chip(p.sdcard ? "SD card" : "No SD card", p.sdcard);
  if (p.printType) chip("Print: " + p.printType, true);
  if (p.speedLevel) chip("Speed " + ["", "Silent", "Standard", "Sport", "Ludicrous"][p.speedLevel], true);
  if (p.wifiSignal) chip("Printer WiFi " + p.wifiSignal + " dBm", p.wifiSignal > -60);
  if (p.ams && p.ams.present) chip("AMS tray " + (p.ams.trayNow >= 0 ? p.ams.trayNow + 1 : "—") + (p.ams.humidity ? " · hum " + p.ams.humidity : ""), true);

  /* controller */
  var rssi = num(d.rssi, -100);
  $("#d-rssi").textContent = rssi + " dBm";
  $("#d-rssibar").style.width = clamp((rssi + 100) * 1.8, 3, 100) + "%";
  $("#d-rssibar").style.background = rssi > -60 ? "var(--acc)" : rssi > -75 ? "var(--warn)" : "var(--err)";
  $("#d-ip").textContent = d.ip || "—";
  $("#d-mdns").textContent = d.mdns || "—";
  $("#d-mac").textContent = d.mac || "—";
  $("#d-up").textContent = fmtDur(num(d.uptimeSec, 0));
  $("#d-heap").textContent = Math.round(num(d.heapFree, 0) / 1024) + " / " + Math.round(num(d.heapMin, 0) / 1024) + " kB";
  var mp = m.printer || {}, mx = m.external || {};
  $("#d-mq1").textContent = (mp.connected ? "connected" : (mp.stateText || "disconnected")) + (mp.reconnects ? " · " + mp.reconnects + " retries" : "");
  $("#d-mq2").textContent = !mx.enabled ? "disabled" : (mx.connected ? "connected" : (mx.stateText || "disconnected"));
  var cm = $("#conn-mqttstate");
  if (cm) { cm.textContent = $("#d-mq2").textContent; cm.style.color = mx.enabled && !mx.connected ? "var(--err)" : ""; }

  /* timers */
  $("#d-tfin").textContent = t.finishActive ? (t.finishRemainingSec ? fmtDur(t.finishRemainingSec) + " left" : "active until door") : "idle";
  $("#d-tinact").textContent = t.inactivityRemainingSec ? fmtDur(t.inactivityRemainingSec) : "—";
  $("#d-tflags").textContent = [t.idleOff ? "idle-off" : "", t.doorToggleOff ? "door-off" : ""].filter(Boolean).join(", ") || "none";

  /* HMS */
  var hl = $("#d-hms"); hl.innerHTML = "";
  var hms = p.hms || [];
  $("#d-hmsempty").hidden = hms.length > 0;
  hms.forEach(function (h) {
    var sev = { Fatal: 1, Serious: 2, Common: 3, Info: 4 }[h.severity] || 4;
    var url = "https://wiki.bambulab.com/en/x1/troubleshooting/hmscode/" +
      String(h.code).replace(/^HMS_/, "").replace(/_/g, "-").toLowerCase();
    var add = el("button", { class: "btn sm", type: "button", text: "+ ignore", title: "Add this code to the ignore list and save" });
    add.onclick = function () { ignoreCode(h.code, add); };
    hl.appendChild(el("li", { class: h.ignored ? "ig" : "" }, [
      el("span", { class: "badge s" + sev, text: h.severity }),
      el("a", { class: "mono", href: url, target: "_blank", rel: "noopener", text: String(h.code) }),
      el("span", { class: "mod", text: h.module || "" }),
      el("span", { class: "sp" }),
      h.ignored ? el("span", { class: "small muted", text: "ignored" }) : add
    ]));
  });
  $("#d-perr").textContent = p.printError ? "print_error " + p.printError : "";
}
function ignoreCode(code, btn) {
  var list = String(cfg.hmsIgnoreList || "").split(",").filter(Boolean);
  var c = String(code).toUpperCase().replace(/-/g, "_");
  if (list.indexOf(c) >= 0) { toast("Already ignored"); return; }
  list.push(c);
  btn.disabled = true;
  put({ hmsIgnoreList: list.join(",") }).then(function (fresh) {
    adoptConfig(fresh); rerenderAll(); toast(c + " added to the ignore list", "ok");
  }).catch(function (e) { toast("Failed: " + e.message, "err"); btn.disabled = false; });
}

/* --------------------------------------------------------- animation loop */
function tick() {
  var now = Date.now();
  var l = (status && status.led) || null;
  if (l) {
    var speed = num(draft.effectSpeed, 5);
    var base;
    if (l.effect === "rainbow") base = hsl2rgb((now / period("rainbow", speed) * 360) % 360);
    else base = composite(l.r, l.g, l.b, l.ww, l.cw);
    var m = modulation(l.effect, speed, now);
    var css = rgbCss(base, m);
    var leds = $("#d-leds");
    if (leds.childElementCount !== LEDCOUNT) {
      leds.innerHTML = "";
      for (var i = 0; i < LEDCOUNT; i++) leds.appendChild(el("i"));
    }
    $$("i", leds).forEach(function (n) { n.style.background = css; });
    $("#d-strip").firstElementChild.style.background =
      "radial-gradient(120% 140% at 50% 120%, " + rgbCss(base, m * 0.55) + " 0%, #000 78%)";
  }
  /* effect / visual preview chips */
  $$("[data-fx]").forEach(function (n) {
    var kind = n.dataset.fx, v = n.dataset.val || "solid", sp = num(draft.effectSpeed, 5);
    if (kind === "effect") {
      var rc = composite.apply(null, hex2rgb(draft.runningRGB).concat([num(draft.runningWW, 0), num(draft.runningCW, 0)]));
      n.style.background = rgbCss(rc, 0.35 + 0.65 * modulation(v, sp, now));
    } else if (kind === "printing") {
      var a = composite.apply(null, hex2rgb(draft.runningRGB).concat([num(draft.runningWW, 0), num(draft.runningCW, 0)]));
      var b = composite.apply(null, hex2rgb(draft.finishRGB).concat([num(draft.finishWW, 0), num(draft.finishCW, 0)]));
      var ph = (now % 8000) / 8000;
      if (v === "progress") n.style.background = rgbCss([a[0] + (b[0] - a[0]) * ph, a[1] + (b[1] - a[1]) * ph, a[2] + (b[2] - a[2]) * ph]);
      else n.style.background = rgbCss(a, v === "breathe" ? modulation("breathe", sp, now) : 1);
    } else if (kind === "preheat") {
      var c = composite.apply(null, hex2rgb(draft.runningRGB).concat([num(draft.runningWW, 0), num(draft.runningCW, 0)]));
      var r2 = (now % 6000) / 6000;
      n.style.background = v === "tempglow"
        ? rgbCss([c[0] * (0.15 + 0.85 * r2) + (r2 < 0.3 ? 60 : 0), c[1] * (0.15 + 0.85 * r2), c[2] * (0.15 + 0.85 * r2)])
        : rgbCss(c);
    }
  });
  requestAnimationFrame(tick);
}

/* ------------------------------------------------------------- dashboard UI */
function wireDash() {
  var seg = $("#d-mode");
  MODES.forEach(function (o) {
    var b = el("button", { type: "button", text: o[1], "data-v": o[0], "aria-pressed": "false" });
    b.onclick = function () {
      post("/api/led/mode", { mode: o[0] })
        .then(function () { draft.ledMode = cfg.ledMode = o[0]; rerenderAll(); toast("Mode: " + o[1], "ok"); })
        .catch(function (e) { toast(e.message, "err"); });
    };
    seg.appendChild(b);
  });

  var bt = $("#d-bright"), btTimer = null;
  bt.oninput = function () {
    brightHold = Date.now();
    $("#d-brightv").textContent = bt.value + " %";
    clearTimeout(btTimer);
    btTimer = setTimeout(function () {
      post("/api/led/brightness", { brightness: parseInt(bt.value, 10) })
        .then(function () { draft.brightness = cfg.brightness = parseInt(bt.value, 10); rerenderAll(); })
        .catch(function (e) { toast(e.message, "err"); });
    }, 350);
  };

  EFFECTS.forEach(function (o) { $("#d-oveffect").appendChild(el("option", { value: o[0], text: o[1] })); });

  /* override colour control reuses ColorField against scratch keys */
  draft.__ovRGB = "#ffffff"; draft.__ovWW = 0; draft.__ovCW = 0;
  $("#d-ovcolor").appendChild(colorField({ k: "__ov", base: "__ov", l: "Override colour", t: "color" }));

  $("#d-ovapply").onclick = function () {
    var mins = clamp(parseInt($("#d-ovmins").value, 10) || 0, 0, 1440);
    post("/api/led", {
      hex: draft.__ovRGB, ww: num(draft.__ovWW, 0), cw: num(draft.__ovCW, 0),
      effect: $("#d-oveffect").value, durationSec: mins * 60
    }).then(function () { toast(mins ? "Override for " + mins + " min" : "Override until cleared", "ok"); })
      .catch(function (e) { toast(e.message, "err"); });
  };
  $("#d-ovclear").onclick = function () {
    api("/api/led", { method: "DELETE" }).then(function () { toast("Override cleared", "ok"); })
      .catch(function (e) { toast(e.message, "err"); });
  };
  $("#d-identify").onclick = function () {
    post("/api/led/identify").then(function () { toast("Identifying", "ok"); }).catch(function (e) { toast(e.message, "err"); });
  };
}

/* ------------------------------------------------------------------ system */
function loadInfo() {
  api("/api/info").then(function (i) {
    info = i;
    var dl = $("#y-info"); dl.innerHTML = "";
    var rows = [
      ["Firmware", (i.fw || "") + (i.codename ? " “" + i.codename + "”" : "")],
      ["Build", i.build], ["Chip", (i.chip || "") + (i.chipRev !== undefined ? " rev " + i.chipRev : "") + (i.cores ? " · " + i.cores + " cores" : "")],
      ["Arduino / SDK", i.sdk],
      ["Flash", i.flashSize ? Math.round(i.flashSize / 1048576) + " MB" : null],
      ["Sketch used / free", i.sketchSize ? Math.round(i.sketchSize / 1024) + " kB / " + Math.round(i.sketchFree / 1024) + " kB" : null],
      ["LED pins", i.pins ? "R" + i.pins.r + " G" + i.pins.g + " B" + i.pins.b + " WW" + i.pins.ww + " CW" + i.pins.cw : null]
    ];
    rows.forEach(function (r) {
      if (r[1] === null || r[1] === undefined || r[1] === "") return;
      dl.appendChild(el("dt", { text: r[0] })); dl.appendChild(el("dd", { text: String(r[1]) }));
    });
    if (i.libs) {
      dl.appendChild(el("dt", { text: "Libraries" }));
      dl.appendChild(el("dd", { class: "small", text: Object.keys(i.libs).map(function (k) { return k + " " + i.libs[k]; }).join(", ") }));
    }
  }).catch(function () {});
}
function upload(url, field, file, progEl, statEl, done) {
  var xhr = new XMLHttpRequest(), fd = new FormData();
  fd.append(field, file, file.name);
  progEl.hidden = false; progEl.value = 0;
  xhr.upload.onprogress = function (e) {
    if (e.lengthComputable) { progEl.value = e.loaded / e.total * 100; statEl.textContent = Math.round(e.loaded / e.total * 100) + "% uploaded"; }
  };
  xhr.onload = function () {
    var ok = xhr.status >= 200 && xhr.status < 300;
    statEl.textContent = ok ? "Upload complete." : ("Failed: HTTP " + xhr.status + " " + xhr.responseText);
    statEl.style.color = ok ? "" : "var(--err)";
    done(ok);
  };
  xhr.onerror = function () { statEl.textContent = "Upload failed (connection lost)."; statEl.style.color = "var(--err)"; done(false); };
  xhr.open("POST", url); xhr.send(fd);
}
function wireSystem() {
  var f1 = $("#y-fwfile"), g1 = $("#y-fwgo");
  f1.onchange = function () { g1.disabled = !f1.files.length; };
  g1.onclick = function () {
    g1.disabled = true;
    upload("/api/update", "firmware", f1.files[0], $("#y-fwprog"), $("#y-fwstat"), function (ok) {
      if (ok) { $("#y-fwstat").textContent = "Flashed. The controller is restarting — reload this page in about 20 seconds."; toast("Firmware uploaded", "ok"); }
      else g1.disabled = false;
    });
  };
  var f2 = $("#y-rsfile"), g2 = $("#y-rsgo");
  f2.onchange = function () { g2.disabled = !f2.files.length; };
  g2.onclick = function () {
    g2.disabled = true;
    upload("/api/config/restore", "file", f2.files[0], $("#y-fwprog"), $("#y-rsstat"), function (ok) {
      if (ok) { showRestart(); toast("Configuration restored", "ok"); }
      g2.disabled = !ok;
    });
  };
  $("#y-reset").onclick = function () {
    confirmBox("Factory reset",
      "This erases every setting including the WiFi credentials, printer access code and web login. The controller reboots into its BLLED-Setup access point and you will have to set it up again.",
      "Erase everything").then(function (yes) {
        if (!yes) return;
        post("/api/config/reset").then(function () { toast("Reset — rebooting into setup mode", "ok"); }).catch(function (e) { toast(e.message, "err"); });
      });
  };
}

/* -------------------------------------------------------------- bootstrap */
function wireGlobal() {
  document.addEventListener("click", function (e) {
    var b = e.target.closest && e.target.closest("[data-save],[data-revert],[data-act]");
    if (!b) return;
    if (b.dataset.save !== undefined) { e.preventDefault(); saveSection(b.dataset.save); return; }
    if (b.dataset.revert !== undefined) {
      e.preventDefault();
      Object.keys(cfg).forEach(function (k) { draft[k] = cfg[k]; });
      rerenderAll(); toast("Reverted");
      return;
    }
    var a = b.dataset.act;
    if (a === "restart") {
      confirmBox("Restart", "The controller reboots; the LEDs go dark for a few seconds.", "Restart").then(function (y) {
        if (y) post("/api/action", { action: "restart" }).then(function () { toast("Restarting…", "ok"); $("#banner").hidden = true; restartNeeded = false; }).catch(function (er) { toast(er.message, "err"); });
      });
      return;
    }
    var body = { action: a };
    if (b.dataset.on !== undefined) body.on = b.dataset.on === "1";
    post("/api/action", body).then(function () { toast(a + " sent", "ok"); }).catch(function (er) { toast(er.message, "err"); });
  });
  $("#b-restart").onclick = function () {
    post("/api/action", { action: "restart" }).then(function () { toast("Restarting…", "ok"); $("#banner").hidden = true; restartNeeded = false; }).catch(function (e) { toast(e.message, "err"); });
  };
  $("#b-dismiss").onclick = function () { $("#banner").hidden = true; };
  window.addEventListener("beforeunload", function (e) {
    var n = SECTIONS.reduce(function (a, s) { return a + (s.id === "dash" ? 0 : dirtyKeys(s).length); }, 0);
    if (n) { e.preventDefault(); e.returnValue = ""; }
  });
  window.addEventListener("hashchange", function () {
    var id = location.hash.slice(1);
    if (SECTIONS.some(function (s) { return s.id === id; })) go(id);
  });
}

function boot() {
  wireTips(); buildNav(); wireGlobal(); wireDash(); wireSystem();
  requestAnimationFrame(tick);
  var start = location.hash.slice(1);
  go(SECTIONS.some(function (s) { return s.id === start; }) ? start : "dash");
  api("/api/config").then(function (c) {
    adoptConfig(c); rerenderAll();
  }).catch(function (e) {
    toast("Could not load configuration: " + e.message, "err");
    rerenderAll();
  });
  loadInfo();
  api("/api/status").then(onStatus).catch(function () { setLive("bad", "offline"); });
  connectWs();
}
if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", boot); else boot();
})();
