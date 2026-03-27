#pragma once
static const char WEB_UI_HTML[] = R"html(<!DOCTYPE html>
<html lang="en" data-theme="dark" data-accent="cyan">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Lights Driver</title>
<style>
*, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

/* ── Theme variables ── */
:root {
  --bg:        #0a0a14;
  --surf:      #141422;
  --surf2:     #1c1c2e;
  --border:    rgba(255,255,255,0.08);
  --bh:        rgba(255,255,255,0.15);
  --text:      #e2e8f0;
  --muted:     #64748b;
  --dim:       #94a3b8;
  --ok:        #10b981;
  --warn:      #f59e0b;
  --err:       #ef4444;
  --radius:    12px;
  --radius-sm: 8px;
  --nav-h:     52px;
  --tr:        0.18s ease;
}
html[data-accent="cyan"]   { --a: #00d4ff; --a2: #0099bb; --ag: rgba(0,212,255,0.12); }
html[data-accent="blue"]   { --a: #4f8ef7; --a2: #2563eb; --ag: rgba(79,142,247,0.12); }
html[data-accent="green"]  { --a: #22d3a0; --a2: #059669; --ag: rgba(34,211,160,0.12); }
html[data-accent="orange"] { --a: #fb923c; --a2: #ea580c; --ag: rgba(251,146,60,0.12); }
html[data-accent="rose"]   { --a: #fb7185; --a2: #e11d48; --ag: rgba(251,113,133,0.12); }

html[data-theme="light"] {
  --bg:    #f0f2f8;
  --surf:  #ffffff;
  --surf2: #e8eaf2;
  --border:rgba(0,0,0,0.09);
  --bh:    rgba(0,0,0,0.16);
  --text:  #1e293b;
  --muted: #94a3b8;
  --dim:   #64748b;
}

html, body {
  height: 100%;
  background: var(--bg);
  color: var(--text);
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
  font-size: 14px;
  line-height: 1.5;
  overflow-x: hidden;
}

::-webkit-scrollbar { width: 5px; }
::-webkit-scrollbar-track { background: transparent; }
::-webkit-scrollbar-thumb { background: rgba(128,128,128,0.3); border-radius: 3px; }

/* ── Top nav ── */
#topbar {
  position: fixed; top: 0; left: 0; right: 0;
  height: var(--nav-h);
  background: var(--surf);
  border-bottom: 1px solid var(--border);
  display: flex; align-items: center; gap: 4px;
  padding: 0 12px;
  z-index: 100;
}
#topbar .logo { font-weight: 700; color: var(--a); margin-right: auto; font-size: 15px; letter-spacing: .5px; }
.nav-btn {
  padding: 6px 14px; border-radius: var(--radius-sm);
  border: none; background: transparent; color: var(--dim);
  cursor: pointer; font-size: 13px; font-weight: 500;
  transition: color var(--tr), background var(--tr);
}
.nav-btn:hover { color: var(--text); background: var(--ag); }
.nav-btn.active { color: var(--a); background: var(--ag); }

/* theme/accent controls */
#theme-toggle {
  width: 32px; height: 32px; border-radius: 50%; border: 1px solid var(--border);
  background: var(--surf2); color: var(--dim); cursor: pointer;
  font-size: 16px; display: flex; align-items: center; justify-content: center;
  margin-left: 8px; transition: border-color var(--tr), color var(--tr);
}
#theme-toggle:hover { border-color: var(--a); color: var(--a); }
.accent-dots { display: flex; gap: 6px; margin-left: 8px; }
.accent-dot {
  width: 16px; height: 16px; border-radius: 50%; cursor: pointer;
  border: 2px solid transparent; transition: border-color var(--tr), transform var(--tr);
}
.accent-dot:hover { transform: scale(1.2); }
.accent-dot.active { border-color: var(--text); }

/* ── Hamburger button (mobile only) ── */
#hamburger {
  display: none; flex-direction: column; justify-content: center; gap: 5px;
  width: 36px; height: 36px; padding: 6px; border-radius: var(--radius-sm);
  border: 1px solid var(--border); background: var(--surf2); cursor: pointer;
  margin-left: 8px;
}
#hamburger span { display: block; height: 2px; background: var(--dim); border-radius: 1px; transition: background var(--tr); }
#hamburger:hover span { background: var(--a); }

/* ── Mobile dropdown menu ── */
#mobile-menu {
  display: none; position: fixed; top: var(--nav-h); left: 0; right: 0;
  background: var(--surf); border-bottom: 1px solid var(--border);
  padding: 8px 12px 12px; z-index: 99; flex-direction: column; gap: 4px;
}
#mobile-menu.open { display: flex; }
#mobile-menu .nav-btn { text-align: left; width: 100%; }
#mobile-menu .mob-divider { height: 1px; background: var(--border); margin: 6px 0; }
#mobile-menu .mob-row { display: flex; align-items: center; gap: 10px; padding: 4px 0; flex-wrap: wrap; }
#mobile-menu .mob-row label { font-size: 13px; color: var(--dim); min-width: 56px; }
#mobile-menu .accent-dots { margin-left: 0; flex-wrap: wrap; }

/* ── Time input: fix dark lights icon ── */
input[type=time] { color-scheme: dark; }
html[data-theme="light"] input[type=time] { color-scheme: light; }

/* ── Responsive: collapse nav on small screens ── */
@media (max-width: 700px) {
  #nav-links, #theme-toggle, #topbar .accent-dots { display: none; }
  #hamburger { display: flex; }
}

/* ── Main content ── */
#main {
  margin-top: var(--nav-h);
  padding: 16px;
  max-width: 640px;
  margin-left: auto; margin-right: auto;
}

/* ── Sections ── */
.section { display: none; flex-direction: column; gap: 12px; }
.section.active { display: flex; }

/* ── Cards ── */
.card {
  background: var(--surf);
  border: 1px solid var(--border);
  border-radius: var(--radius);
  padding: 16px;
}
.card-title {
  font-size: 11px; font-weight: 600; letter-spacing: .8px;
  text-transform: uppercase; color: var(--muted);
  margin-bottom: 12px;
}

/* ── Buttons ── */
.btn {
  padding: 8px 16px; border-radius: var(--radius-sm);
  border: none; cursor: pointer; font-size: 13px; font-weight: 500;
  transition: opacity var(--tr), background var(--tr);
}
.btn:active { opacity: .75; }
.btn-primary { background: var(--a); color: #000; }
.btn-primary:hover { background: var(--a2); }
.btn-secondary {
  background: var(--surf2); color: var(--text);
  border: 1px solid var(--border);
}
.btn-secondary:hover { border-color: var(--bh); }
.btn-sm { padding: 5px 12px; font-size: 12px; }
.btn-full { width: 100%; }

/* ── Lights ── */
.strip-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 12px; }
.strip-name { font-size: 14px; font-weight: 600; }
.swatch-box {
  width: 28px; height: 28px; border-radius: 6px;
  border: 1px solid var(--border); display: inline-block;
}
.effect-info { font-size: 12px; color: var(--dim); }
.pct-info { font-size: 12px; color: var(--a); font-weight: 600; }

.slider-row { display: flex; align-items: center; gap: 10px; margin: 10px 0; }
.slider-row label { font-size: 11px; color: var(--muted); width: 70px; flex-shrink: 0; }
input[type=range] {
  flex: 1; -webkit-appearance: none; height: 4px;
  border-radius: 2px; background: var(--surf2);
  outline: none; cursor: pointer;
}
input[type=range]::-webkit-slider-thumb {
  -webkit-appearance: none; width: 16px; height: 16px;
  border-radius: 50%; background: var(--a); cursor: pointer;
}

.fx-grid { display: flex; flex-wrap: wrap; gap: 6px; margin: 10px 0; }
.fx-btn {
  padding: 5px 10px; border-radius: 6px; border: 1px solid var(--border);
  background: var(--surf2); color: var(--dim); cursor: pointer;
  font-size: 12px; transition: all var(--tr);
}
.fx-btn:hover { border-color: var(--a); color: var(--a); }
.fx-btn.active { background: var(--ag); border-color: var(--a); color: var(--a); }

.color-grid { display: grid; grid-template-columns: repeat(7, 1fr); gap: 6px; margin-top: 10px; }
.color-swatch {
  aspect-ratio: 1; border-radius: 6px; cursor: pointer;
  border: 2px solid transparent; transition: border-color var(--tr), transform var(--tr);
}
.color-swatch:hover { transform: scale(1.1); }
.color-swatch.active { border-color: white; }
.color-sect-label { font-size: 10px; color: var(--muted); margin-top: 10px; margin-bottom: 2px; letter-spacing: .5px; }

/* ── Info grid ── */
.info-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
.info-item { background: var(--surf2); border-radius: var(--radius-sm); padding: 10px; }
.info-label { font-size: 10px; color: var(--muted); text-transform: uppercase; letter-spacing: .6px; }
.info-val { font-size: 14px; font-weight: 500; margin-top: 2px; word-break: break-all; }
.info-item.wide { grid-column: 1/-1; }

.pin-table { width: 100%; border-collapse: collapse; font-size: 12px; }
.pin-table th { text-align: left; padding: 6px 8px; color: var(--muted); font-weight: 500; border-bottom: 1px solid var(--border); }
.pin-table td { padding: 6px 8px; border-bottom: 1px solid var(--border); }
.pin-table tr:last-child td { border-bottom: none; }
.pin-table .pin-num { font-family: monospace; color: var(--a); }

/* ── Config ── */
.cfg-row { display: flex; align-items: center; gap: 8px; margin: 8px 0; }
.cfg-row label { font-size: 13px; color: var(--dim); flex: 1; }
.cfg-row input[type=number] {
  width: 80px; padding: 6px 8px; border-radius: var(--radius-sm);
  border: 1px solid var(--border); background: var(--surf2);
  color: var(--text); font-size: 14px; text-align: center;
}
.cfg-row input[type=number]:focus { outline: none; border-color: var(--a); }

.dir-toggle { display: flex; gap: 0; border-radius: var(--radius-sm); overflow: hidden; border: 1px solid var(--border); }
.dir-opt {
  flex: 1; padding: 7px; text-align: center; cursor: pointer;
  font-size: 12px; font-weight: 500; color: var(--muted);
  background: var(--surf2); border: none;
  transition: background var(--tr), color var(--tr);
}
.dir-opt.active { background: var(--ag); color: var(--a); }

.offset-ctrl { display: flex; align-items: center; gap: 8px; }
.offset-val { font-size: 20px; font-weight: 300; min-width: 48px; text-align: center; font-variant-numeric: tabular-nums; }

/* ── Toast ── */
#toast {
  position: fixed; bottom: 20px; left: 50%; transform: translateX(-50%) translateY(60px);
  background: var(--surf); border: 1px solid var(--border);
  padding: 10px 20px; border-radius: 20px; font-size: 13px;
  transition: transform .25s ease, opacity .25s ease; opacity: 0;
  pointer-events: none; z-index: 999; white-space: nowrap;
}
#toast.show { transform: translateX(-50%) translateY(0); opacity: 1; }
#toast.ok { border-color: var(--ok); color: var(--ok); }
#toast.err { border-color: var(--err); color: var(--err); }
</style>
</head>
<body>

<!-- ── Top bar ─────────────────────────────────────────────────────────── -->
<div id="topbar">
  <span class="logo">&#9201; Lights</span>
  <div id="nav-links" style="display:flex;align-items:center;gap:4px;">
    <button class="nav-btn active" onclick="nav('lights')">Lights</button>
    <button class="nav-btn" onclick="nav('info')">Info</button>
    <button class="nav-btn" onclick="nav('config')">Config</button>
  </div>
  <button id="theme-toggle" onclick="toggleTheme()" title="Toggle light/dark">&#9788;</button>
  <div class="accent-dots">
    <div class="accent-dot active" style="background:#00d4ff" data-accent="cyan"    onclick="setAccent(this)"></div>
    <div class="accent-dot"        style="background:#4f8ef7" data-accent="blue"    onclick="setAccent(this)"></div>
    <div class="accent-dot"        style="background:#22d3a0" data-accent="green"   onclick="setAccent(this)"></div>
    <div class="accent-dot"        style="background:#fb923c" data-accent="orange"  onclick="setAccent(this)"></div>
    <div class="accent-dot"        style="background:#fb7185" data-accent="rose"    onclick="setAccent(this)"></div>
  </div>
  <button id="hamburger" onclick="toggleMobileMenu()" aria-label="Menu">
    <span></span><span></span><span></span>
  </button>
</div>

<!-- ── Mobile dropdown ──────────────────────────────────────────────────── -->
<div id="mobile-menu">
  <button class="nav-btn active" onclick="nav('lights');closeMobileMenu()">Lights</button>
  <button class="nav-btn" onclick="nav('info');closeMobileMenu()">Info</button>
  <button class="nav-btn" onclick="nav('config');closeMobileMenu()">Config</button>
  <div class="mob-divider"></div>
  <div class="mob-row">
    <label>Theme</label>
    <button id="theme-toggle-mob" onclick="toggleTheme()" title="Toggle light/dark" style="width:32px;height:32px;border-radius:50%;border:1px solid var(--border);background:var(--surf2);color:var(--dim);cursor:pointer;font-size:16px;">&#9788;</button>
  </div>
  <div class="mob-row">
    <label>Accent</label>
    <div class="accent-dots">
      <div class="accent-dot active" style="background:#00d4ff" data-accent="cyan"    onclick="setAccent(this)"></div>
      <div class="accent-dot"        style="background:#4f8ef7" data-accent="blue"    onclick="setAccent(this)"></div>
      <div class="accent-dot"        style="background:#22d3a0" data-accent="green"   onclick="setAccent(this)"></div>
      <div class="accent-dot"        style="background:#fb923c" data-accent="orange"  onclick="setAccent(this)"></div>
      <div class="accent-dot"        style="background:#fb7185" data-accent="rose"    onclick="setAccent(this)"></div>
    </div>
  </div>
</div>

<div id="main">

<!-- ════════════════════════════════════ LIGHTS ══════════════════════════ -->
<div id="sec-lights" class="section active">

  <div class="card" id="strip1-card">
    <div class="strip-header">
      <span class="strip-name">Lights</span>
      <div style="display:flex;align-items:center;gap:8px;">
        <span class="swatch-box" id="sw1"></span>
        <span class="effect-info" id="fx1-label">—</span>
        <span class="pct-info" id="bright1-pct">—</span>
      </div>
    </div>
    <div class="slider-row">
      <label>Brightness</label>
      <input type="range" id="bright1" min="0" max="255" value="128"
             onpointerdown="sliderActive[0]=true" oninput="doBright(1,this.value)">
    </div>
    <div class="card-title" style="margin-top:10px;">Effects</div>
    <div class="fx-grid" id="fx1-grid">
      <button class="fx-btn" data-fx1="Static"   onclick="doFx(1,'static')">Static</button>
      <button class="fx-btn" data-fx1="Breathe"  onclick="doFx(1,'breathe')">Breathe</button>
      <button class="fx-btn" data-fx1="Rainbow"  onclick="doFx(1,'rainbow')">Rainbow</button>
      <button class="fx-btn" data-fx1="Chase"    onclick="doFx(1,'chase')">Chase</button>
      <button class="fx-btn" data-fx1="Sparkle"  onclick="doFx(1,'sparkle')">Sparkle</button>
      <button class="fx-btn" data-fx1="Wipe"     onclick="doFx(1,'wipe')">Wipe</button>
      <button class="fx-btn" data-fx1="Comet"    onclick="doFx(1,'comet')">Comet</button>
    </div>
    <div class="card-title" style="margin-top:10px;">Colour</div>
    <div class="color-grid" id="color1-grid"></div>
  </div>


</div><!-- /sec-lights -->

<!-- ════════════════════════════════════ INFORMATION ═════════════════════ -->
<div id="sec-info" class="section">

  <!-- System -->
  <div class="card">
    <div class="card-title">System</div>
    <div class="info-grid">
      <div class="info-item"><div class="info-label">Firmware</div><div class="info-val" id="inf-fw">—</div></div>
      <div class="info-item"><div class="info-label">Uptime</div><div class="info-val" id="inf-uptime">—</div></div>
      <div class="info-item"><div class="info-label">Free Heap</div><div class="info-val" id="inf-heap">—</div></div>
    </div>
  </div>

  <!-- Network -->
  <div class="card">
    <div class="card-title">Network</div>
    <div class="info-grid">
      <div class="info-item"><div class="info-label">WiFi SSID</div><div class="info-val" id="inf-ssid">—</div></div>
      <div class="info-item"><div class="info-label">Signal (RSSI)</div><div class="info-val" id="inf-rssi">—</div></div>
      <div class="info-item"><div class="info-label">Local IP</div><div class="info-val" id="inf-lip">—</div></div>
      <div class="info-item"><div class="info-label">Gateway</div><div class="info-val" id="inf-gw">—</div></div>
      <div class="info-item"><div class="info-label">Internet IP</div><div class="info-val" id="inf-eip">—</div></div>
      <div class="info-item"><div class="info-label">Location</div><div class="info-val" id="inf-geo">—</div></div>
    </div>
  </div>

  <!-- LED Strips -->
  <div class="card">
    <div class="card-title">LED Strips</div>
    <div class="info-grid">
      <div class="info-item">
        <div class="info-label">Left</div>
        <div class="info-val" id="inf-s1" style="display:flex;align-items:center;gap:6px;"></div>
      </div>
      <div class="info-item">
        <div class="info-label">Right</div>
        <div class="info-val" id="inf-s2" style="display:flex;align-items:center;gap:6px;"></div>
      </div>
      <div class="info-item"><div class="info-label">Left LEDs</div><div class="info-val" id="inf-s1len">—</div></div>
      <div class="info-item"><div class="info-label">Right LEDs</div><div class="info-val" id="inf-s2len">—</div></div>
    </div>
  </div>

  <!-- GPIO Map -->
  <div class="card">
    <div class="card-title">GPIO Pin Map</div>
    <table class="pin-table">
      <thead><tr><th>GPIO</th><th>Function</th></tr></thead>
      <tbody>
        <tr><td class="pin-num">8</td><td>I2C SDA (Display)</td></tr>
        <tr><td class="pin-num">9</td><td>I2C SCL (Display)</td></tr>
        <tr><td class="pin-num">10</td><td>Button A (Menu Prev)</td></tr>
        <tr><td class="pin-num">11</td><td>Button B (Menu Next)</td></tr>
        <tr><td class="pin-num">LED1</td><td>WS2812B Left Data</td></tr>
        <tr><td class="pin-num">LED2</td><td>WS2812B Right Data</td></tr>
      </tbody>
    </table>
  </div>

  <!-- Timezone -->
  <div class="card">
    <div class="card-title">Timezone</div>
    <div class="info-grid">
      <div class="info-item"><div class="info-label">IANA TZ</div><div class="info-val" id="inf-tz-iana">—</div></div>
      <div class="info-item"><div class="info-label">POSIX TZ</div><div class="info-val" id="inf-tz-posix" style="font-size:12px;font-family:monospace;">—</div></div>
    </div>
  </div>

</div><!-- /sec-info -->

<!-- ════════════════════════════════════ CONFIGURATION ═══════════════════ -->
<div id="sec-config" class="section">

  <!-- Strip Lengths -->
  <div class="card">
    <div class="card-title">LED Strip Lengths</div>
    <div class="cfg-row">
      <label>Left LED count</label>
      <input type="number" id="cfg-s1len" min="1" max="300" value="24">
      <button class="btn btn-secondary btn-sm" onclick="applyLen(1)">Apply</button>
    </div>
    <div class="cfg-row">
      <label>Right LED count</label>
      <input type="number" id="cfg-s2len" min="1" max="300" value="6">
      <button class="btn btn-secondary btn-sm" onclick="applyLen(2)">Apply</button>
    </div>
  </div>

  <!-- Timezone Override -->
  <div class="card">
    <div class="card-title">Timezone Override</div>
    <p style="font-size:12px;color:var(--muted);margin-bottom:10px;">
      Leave empty to use automatic geolocation. Format: POSIX TZ string, e.g.
      <code style="font-size:11px;color:var(--a)">CST6CDT,M3.2.0,M11.1.0</code>
    </p>
    <div class="cfg-row">
      <input type="text" id="cfg-tz" placeholder="Empty = auto-detect"
             style="flex:1;padding:6px 10px;border-radius:var(--radius-sm);border:1px solid var(--border);
                    background:var(--surf2);color:var(--text);font-size:12px;font-family:monospace;">
      <button class="btn btn-secondary btn-sm" onclick="saveTz()">Save</button>
    </div>
  </div>

  <!-- Device Hostname (mDNS) -->
  <div class="card">
    <div class="card-title">Device Hostname</div>
    <p style="font-size:12px;color:var(--muted);margin-bottom:10px;">
      Access this device at <span id="cfg-mdns-display" style="color:var(--a);font-family:monospace;">lights_xxxx.local</span>.
      Changes take effect immediately &mdash; no restart required.
    </p>
    <div class="cfg-row">
      <input type="text" id="cfg-mdns" placeholder="lights_xxxx" autocomplete="off"
             style="flex:1;padding:6px 10px;border-radius:var(--radius-sm);border:1px solid var(--border);
                    background:var(--surf2);color:var(--text);font-size:13px;font-family:monospace;">
      <button class="btn btn-secondary btn-sm" onclick="saveMdnsHostname()">Save</button>
    </div>
  </div>

  <!-- WiFi Credentials -->
  <div class="card">
    <div class="card-title">WiFi Credentials</div>
    <p style="font-size:12px;color:var(--muted);margin-bottom:10px;">
      Update the WiFi network name and password. The device will restart to connect
      with the new credentials. Matter commissioning is preserved.
    </p>
    <div class="cfg-row">
      <label style="min-width:72px;font-size:12px;">SSID</label>
      <input type="text" id="cfg-wifi-ssid" placeholder="Network name" autocomplete="off"
             spellcheck="false"
             style="flex:1;padding:6px 10px;border-radius:var(--radius-sm);border:1px solid var(--border);
                    background:var(--surf2);color:var(--text);font-size:13px;">
    </div>
    <div class="cfg-row" style="margin-top:8px;">
      <label style="min-width:72px;font-size:12px;">Password</label>
      <input type="password" id="cfg-wifi-pass" placeholder="Password" autocomplete="new-password"
             style="flex:1;padding:6px 10px;border-radius:var(--radius-sm);border:1px solid var(--border);
                    background:var(--surf2);color:var(--text);font-size:13px;">
    </div>
    <div style="margin-top:12px;display:flex;gap:10px;align-items:center;">
      <button class="btn btn-secondary btn-sm" onclick="saveWifiCreds()">Save &amp; Restart</button>
      <span style="font-size:11px;color:var(--muted);">Connection will be lost during restart</span>
    </div>
  </div>

  <!-- Firmware Update -->
  <div class="card">
    <div class="card-title">Firmware Update</div>
    <div class="info-grid" style="margin-bottom:12px;">
      <div class="info-item">
        <div class="info-label">Running</div>
        <div class="info-val" id="ota-running">—</div>
      </div>
      <div class="info-item">
        <div class="info-label">Latest</div>
        <div class="info-val" id="ota-latest">—</div>
      </div>
    </div>
    <div id="ota-status" style="font-size:12px;margin-bottom:12px;color:var(--muted);min-height:16px;"></div>
    <div style="display:flex;gap:8px;margin-bottom:16px;">
      <button class="btn btn-secondary btn-sm" style="flex:1" onclick="checkForUpdates()">Check for Updates</button>
      <button class="btn btn-sm" id="ota-update-btn" style="flex:1;display:none;" onclick="updateNow()">Update Now</button>
    </div>
    <div style="font-size:12px;font-weight:600;color:var(--dim);margin-bottom:6px;">Auto Update</div>
    <p style="font-size:12px;color:var(--muted);margin-bottom:10px;">
      When enabled, the device checks GitHub for new firmware every 24&nbsp;hours and installs automatically.
    </p>
    <div class="dir-toggle">
      <button class="dir-opt" id="ota-auto-on"  onclick="setAutoUpdate(true)">Enabled</button>
      <button class="dir-opt" id="ota-auto-off" onclick="setAutoUpdate(false)">Disabled</button>
    </div>
  </div>

</div><!-- /sec-config -->
</div><!-- /main -->

<div id="toast"></div>

<script>
// ── 21 Matter-compatible colour presets ──────────────────────────────────────
// Saturated colours are normalised so max channel = 255, matching xy_to_rgb()
// output from Alexa/Matter.  White-balance entries keep baked-in warmth.
const COLORS = [
  {name:'Red',          r:255, g:0,   b:0  },  // H=0°
  {name:'Orange',       r:255, g:128, b:0  },  // H=30°
  {name:'Amber',        r:255, g:176, b:0  },  // H=41°
  {name:'Yellow',       r:255, g:255, b:0  },  // H=60°
  {name:'Lime',         r:128, g:255, b:0  },  // H=90°
  {name:'Green',        r:0,   g:255, b:0  },  // H=120°
  {name:'Spring Green', r:0,   g:255, b:128},  // H=150°
  {name:'Cyan',         r:0,   g:255, b:255},  // H=180°
  {name:'Azure',        r:0,   g:128, b:255},  // H=210°
  {name:'Blue',         r:0,   g:0,   b:255},  // H=240°
  {name:'Indigo',       r:64,  g:0,   b:255},  // H=255°
  {name:'Violet',       r:128, g:0,   b:255},  // H=270°
  {name:'Magenta',      r:255, g:0,   b:255},  // H=300°
  {name:'Pink',         r:255, g:0,   b:128},  // H=330°
  {name:'Rose',         r:255, g:0,   b:64 },  // H=345°
  {name:'Warm White',   r:255, g:198, b:128},  // ~2700 K
  {name:'Soft White',   r:255, g:224, b:160},  // ~3000 K
  {name:'Cool White',   r:208, g:232, b:255},  // ~6000 K
  {name:'Daylight',     r:240, g:240, b:255},  // ~6500 K
  {name:'Coral',        r:255, g:88,  b:64 },
  {name:'Lavender',     r:150, g:123, b:182},
];

// ── State ────────────────────────────────────────────────────────────────────
let brightTimer1 = null;
let lastData = {};

// Config fields that require explicit Save/Apply should only be populated
// once on first load, then only refreshed when the server value actually
// changes (e.g. via UART console) — never during normal 1-second WS pushes.
let cfgInitialized = false;
let serverCfg = {};   // last values seen FROM the server for config fields

// Brightness sliders: skip WS updates while user is actively dragging.
let sliderActive = [false, false];
document.addEventListener('pointerup',     () => { sliderActive = [false, false]; });
document.addEventListener('pointercancel', () => { sliderActive = [false, false]; });

// ── Navigation ───────────────────────────────────────────────────────────────
function nav(id) {
  document.querySelectorAll('.section').forEach(s => s.classList.remove('active'));
  document.querySelectorAll('.nav-btn').forEach(b => b.classList.remove('active'));
  document.getElementById('sec-' + id).classList.add('active');
  // Mark all nav buttons with matching section as active (covers both topbar + mobile menu)
  document.querySelectorAll('.nav-btn').forEach(b => {
    if (b.getAttribute('onclick') && b.getAttribute('onclick').includes("'" + id + "'")) {
      b.classList.add('active');
    }
  });
}

// ── Mobile menu ───────────────────────────────────────────────────────────────
function toggleMobileMenu() {
  document.getElementById('mobile-menu').classList.toggle('open');
}
function closeMobileMenu() {
  document.getElementById('mobile-menu').classList.remove('open');
}
// Close mobile menu when tapping outside
document.addEventListener('click', function(e) {
  const menu = document.getElementById('mobile-menu');
  const btn  = document.getElementById('hamburger');
  if (menu.classList.contains('open') && !menu.contains(e.target) && !btn.contains(e.target)) {
    menu.classList.remove('open');
  }
});

// ── Theme ────────────────────────────────────────────────────────────────────
function toggleTheme() {
  const cur = document.documentElement.getAttribute('data-theme');
  const next = cur === 'dark' ? 'light' : 'dark';
  document.documentElement.setAttribute('data-theme', next);
  localStorage.setItem('theme', next);
}
function setAccent(el) {
  const a = el.dataset.accent;
  document.documentElement.setAttribute('data-accent', a);
  document.querySelectorAll('.accent-dot').forEach(d => d.classList.toggle('active', d.dataset.accent === a));
  localStorage.setItem('accent', a);
}
(function initTheme() {
  const t = localStorage.getItem('theme') || 'dark';
  const a = localStorage.getItem('accent') || 'cyan';
  document.documentElement.setAttribute('data-theme', t);
  document.documentElement.setAttribute('data-accent', a);
  document.querySelectorAll('.accent-dot').forEach(d => d.classList.toggle('active', d.dataset.accent === a));
})();

// ── Toast ────────────────────────────────────────────────────────────────────
let toastTimer;
function toast(msg, type='ok') {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.className = 'show ' + type;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.className = ''; }, 2500);
}

// ── API helpers ──────────────────────────────────────────────────────────────
function post(cmd, extra) {
  return fetch('/api/cmd', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify(Object.assign({cmd}, extra))
  })
  .then(r => r.json())
  .then(d => { if (!d.ok) toast(d.msg || 'Error', 'err'); })
  .catch(() => toast('Network error', 'err'));
}

function postCfg(body) {
  return fetch('/api/cfg', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify(body)
  })
  .then(r => r.json())
  .then(d => { toast(d.ok ? 'Saved' : 'Error', d.ok ? 'ok' : 'err'); })
  .catch(() => toast('Network error', 'err'));
}

function postOta(body) {
  return fetch('/api/ota', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify(body)
  })
  .then(r => r.json())
  .catch(() => { toast('Network error', 'err'); });
}

// ── OTA commands ─────────────────────────────────────────────────────────────
function checkForUpdates() {
  setText('ota-status', 'Checking...');
  setText('ota-latest', '...');
  postOta({action: 'check'}).then(() => {
    toast('Checking for updates — result appears in a few seconds');
  });
}

function updateNow() {
  if (!confirm('Install firmware update now?\nThe device will restart automatically when done.')) return;
  setText('ota-status', 'Downloading update — device will restart when done...');
  document.getElementById('ota-update-btn').style.display = 'none';
  postOta({action: 'update'}).then(() => {
    toast('Update started');
  });
}

function setAutoUpdate(val) {
  document.getElementById('ota-auto-on').classList.toggle('active', val);
  document.getElementById('ota-auto-off').classList.toggle('active', !val);
  postOta({auto_update: val}).then(d => {
    if (d && d.ok) toast(val ? 'Auto update enabled' : 'Auto update disabled');
  });
}

// ── Lights ───────────────────────────────────────────────────────────────────
function buildColorGrid(n) {
  const grid = document.getElementById('color' + n + '-grid');
  grid.innerHTML = '';
  COLORS.forEach((c, i) => {
    const el = document.createElement('div');
    el.className = 'color-swatch';
    el.style.background = 'rgb(' + c.r + ',' + c.g + ',' + c.b + ')';
    el.title = c.name;
    el.dataset.idx = i;
    el.onclick = () => doColor(n, c.r, c.g, c.b, el);
    grid.appendChild(el);
  });
}

function doColor(n, r, g, b, el) {
  document.querySelectorAll('#color' + n + '-grid .color-swatch').forEach(s => s.classList.remove('active'));
  if (el) el.classList.add('active');
  post('led' + n + '-color', {r, g, b});
}

function doFx(n, fx) {
  post('led' + n + '-' + fx);
}

function doBright(n, val) {
  val = parseInt(val);
  document.getElementById('bright' + n + '-pct').textContent = Math.round(val / 255 * 100) + '%';
  clearTimeout(brightTimer1);
  brightTimer1 = setTimeout(() => post('led' + n + '-bright', {brightness: val}), 80);
}

function applyStrip(n, s) {
  const pct = Math.round((s.brightness || 0) / 255 * 100);
  // Skip brightness slider update while user is actively dragging it
  if (!sliderActive[n - 1]) {
    document.getElementById('bright' + n).value = s.brightness || 0;
    document.getElementById('bright' + n + '-pct').textContent = pct + '%';
  }
  document.getElementById('sw' + n).style.background = 'rgb(' + s.r + ',' + s.g + ',' + s.b + ')';
  document.getElementById('fx' + n + '-label').textContent = s.effect || '—';
  // Highlight active effect button
  const attr = 'data-fx' + n;
  document.querySelectorAll('[' + attr + ']').forEach(b => {
    b.classList.toggle('active', b.getAttribute(attr) === s.effect);
  });
  // Highlight matching color swatch
  const grid = document.getElementById('color' + n + '-grid');
  if (grid) {
    grid.querySelectorAll('.color-swatch').forEach(sw => {
      const c = COLORS[parseInt(sw.dataset.idx)];
      const match = c && c.r === s.r && c.g === s.g && c.b === s.b;
      sw.classList.toggle('active', match);
    });
  }
}

// ── Config ───────────────────────────────────────────────────────────────────
function applyLen(n) {
  const v = parseInt(document.getElementById('cfg-s' + n + 'len').value);
  if (isNaN(v) || v < 1) { toast('Invalid length', 'err'); return; }
  const body = {};
  body['strip' + n + '_len'] = v;
  postCfg(body);
}

function doReadSensor() {
  post('read-sensor').then(() => {
    // last_sensor_adc updates via WebSocket within 1s; force a status fetch now
    fetch('/api/status').then(r => r.json()).then(applyData).catch(() => {});
  });
}

let scanPollTimer = null;
function doSensorScan() {
  const btn = document.getElementById('btn-scan');
  btn.disabled = true;
  btn.textContent = 'Scanning…';
  document.getElementById('diag-scan-wrap').style.display = '';
  document.getElementById('diag-scan-table').textContent = '';
  document.getElementById('diag-scan-status').textContent = 'Running scan (~24 s)…';
  post('sensor-scan');
  // Poll /api/scan-results until scanning flag clears
  scanPollTimer = setInterval(() => {
    fetch('/api/scan-results').then(r => r.json()).then(d => {
      if (!d.scanning) {
        clearInterval(scanPollTimer);
        scanPollTimer = null;
        btn.disabled = false;
        btn.textContent = 'Scan Left (20 min)';
        renderScanResults(d);
      }
    }).catch(() => {});
  }, 1500);
}

function renderScanResults(d) {
  const thr = d.threshold || 0;
  const mean = d.mean || 0;
  document.getElementById('diag-scan-status').textContent =
    'Done — ' + (d.results ? d.results.length : 0) + ' points  |  mean=' + mean + '  threshold=' + thr;
  if (!d.results || d.results.length === 0) {
    document.getElementById('diag-scan-table').textContent = '(no data)';
    return;
  }
  let lines = 'step    ADC    trig?   level\n' +
              '─────────────────────────────────────────────────\n';
  d.results.forEach(r => {
    const trig = r.v > thr;
    const bar = (trig ? '#' : '=').repeat(Math.min(40, Math.round(r.v * 40 / 4095)));
    lines += r.s.toString().padEnd(7) + r.v.toString().padEnd(7) +
             (trig ? 'YES <<< ' : 'no      ') + '|' + bar + '\n';
  });
  document.getElementById('diag-scan-table').textContent = lines;
}

function saveTz() {
  const tz = document.getElementById('cfg-tz').value.trim();
  postCfg({tz_override: tz}).then(() => toast('Timezone saved — restart to apply'));
}
function saveWifiCreds() {
  const ssid = (document.getElementById('cfg-wifi-ssid').value || '').trim();
  const pass = document.getElementById('cfg-wifi-pass').value || '';
  if (!ssid) { toast('SSID is required', 'err'); return; }
  if (!confirm('Update WiFi to "' + ssid + '" and restart?')) return;
  postCfg({wifi_ssid: ssid, wifi_password: pass}).then(() => {
    toast('Credentials saved \u2014 device is restarting\u2026');
    document.getElementById('cfg-wifi-ssid').value = '';
    document.getElementById('cfg-wifi-pass').value = '';
  });
}

function saveMdnsHostname() {
  const name = document.getElementById('cfg-mdns').value.trim();
  if (!name) return;
  postCfg({mdns_hostname: name}).then(() => {
    setText('cfg-mdns-display', name + '.local');
    toast('Hostname updated \u2014 now reachable at ' + name + '.local');
  });
}

// ── Data application ─────────────────────────────────────────────────────────
function setText(id, val) {
  const el = document.getElementById(id);
  if (el) el.textContent = val;
}

function fmtUptime(s) {
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  return h + 'h ' + m + 'm ' + sec + 's';
}

function fmtHeap(b) {
  return (b / 1024).toFixed(1) + ' KB';
}

function applyData(d) {
  lastData = d;

  // Lights
  if (d.leds) {
    if (d.leds.strip1) applyStrip(1, d.leds.strip1);
  }

  // Info section
  setText('inf-fw', d.fw_version ? 'v' + d.fw_version : '—');

  // mDNS hostname — populate once on first data; don't overwrite user edits
  if (d.mdns_hostname && !applyData.mdnsDone) {
    applyData.mdnsDone = true;
    document.getElementById('cfg-mdns').value = d.mdns_hostname;
    setText('cfg-mdns-display', d.mdns_hostname + '.local');
  }

  // OTA section
  if (d.ota_running !== undefined) setText('ota-running', 'v' + d.ota_running);
  if (d.ota_latest !== undefined) {
    setText('ota-latest', d.ota_checking ? 'Checking...' : (d.ota_latest ? 'v' + d.ota_latest : '—'));
  }
  if (d.ota_avail) {
    setText('ota-status', '\u26A0 Update available!');
    document.getElementById('ota-update-btn').style.display = '';
    document.getElementById('ota-update-btn').style.background = 'var(--warn)';
    document.getElementById('ota-update-btn').style.color = '#000';
  } else if (d.ota_latest && !d.ota_checking) {
    setText('ota-status', '\u2713 Up to date');
    document.getElementById('ota-update-btn').style.display = 'none';
  } else if (d.ota_checking) {
    setText('ota-status', 'Checking...');
  }
  if (d.ota_auto !== undefined) {
    document.getElementById('ota-auto-on')?.classList.toggle('active', d.ota_auto);
    document.getElementById('ota-auto-off')?.classList.toggle('active', !d.ota_auto);
  }
  setText('inf-uptime', fmtUptime(d.uptime_s || 0));
  setText('inf-heap', fmtHeap(d.free_heap || 0));
  setText('inf-ssid', d.ssid || '—');
  setText('inf-rssi', d.rssi !== undefined ? d.rssi + ' dBm' : '—');
  setText('inf-lip', d.local_ip || '—');
  setText('inf-gw', d.gateway || '—');
  setText('inf-eip', d.external_ip || '—');
  const geo = [d.city, d.region, d.isp].filter(Boolean).join(' · ');
  setText('inf-geo', geo || '—');

  // Strip info
  function stripInfoHTML(s) {
    if (!s) return '—';
    return '<span style="width:14px;height:14px;border-radius:3px;display:inline-block;background:rgb(' + s.r + ',' + s.g + ',' + s.b + ');flex-shrink:0"></span>' +
           '<span>' + (s.effect || '—') + ' · ' + Math.round((s.brightness||0)/255*100) + '%</span>';
  }
  const s1el = document.getElementById('inf-s1');
  const s2el = document.getElementById('inf-s2');
  if (s1el && d.leds && d.leds.strip1) s1el.innerHTML = stripInfoHTML(d.leds.strip1);
  if (s2el && d.leds && d.leds.strip2) s2el.innerHTML = stripInfoHTML(d.leds.strip2);
  setText('inf-s1len', d.leds && d.leds.strip1 ? d.leds.strip1.active_len + ' LEDs' : '—');
  setText('inf-s2len', d.leds && d.leds.strip2 ? d.leds.strip2.active_len + ' LEDs' : '—');

  setText('inf-tz-iana', d.iana_tz || '—');
  setText('inf-tz-posix', d.posix_tz || '—');

  // ── Config fields: init once, then only update on external server-side change ──
  // This prevents WS pushes from stomping on values the user is editing.
  const s1len = d.leds && d.leds.strip1 ? d.leds.strip1.active_len : undefined;
  const s2len = d.leds && d.leds.strip2 ? d.leds.strip2.active_len : undefined;

  if (!cfgInitialized) {
    // First data received — populate all config inputs
    if (s1len !== undefined) document.getElementById('cfg-s1len').value = s1len;
    if (s2len !== undefined) document.getElementById('cfg-s2len').value = s2len;

    // Snapshot initial server state
    serverCfg = { s1len, s2len };
    cfgInitialized = true;

  } else {
    // Subsequent pushes — only update a config input if the server value
    // actually changed (catches external changes via UART/console).
    if (s1len !== undefined && s1len !== serverCfg.s1len) {
      document.getElementById('cfg-s1len').value = s1len;
      serverCfg.s1len = s1len;
    }
    if (s2len !== undefined && s2len !== serverCfg.s2len) {
      document.getElementById('cfg-s2len').value = s2len;
      serverCfg.s2len = s2len;
    }
  }

}

// ── WebSocket ─────────────────────────────────────────────────────────────────
function connectWS() {
  const ws = new WebSocket('ws://' + location.host + '/ws');
  ws.onopen  = () => { cfgInitialized = false; }; // re-populate Config fields on every (re)connect
  ws.onmessage = e => {
    try { applyData(JSON.parse(e.data)); } catch(err) {}
  };
  ws.onclose = () => setTimeout(connectWS, 2000);
}

// ── Init ─────────────────────────────────────────────────────────────────────
buildColorGrid(1);

// Initial fetch
fetch('/api/status').then(r => r.json()).then(applyData).catch(() => {});

connectWS();
</script>
</body>
</html>
)html";
static const size_t WEB_UI_HTML_LEN = sizeof(WEB_UI_HTML) - 1;
