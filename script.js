// =============================================================================
// Canopy Dashboard — script.js
// Polls the backend for live data, OR runs a self-contained simulation in
// Demo mode (no ESP32 / backend required).
// =============================================================================

// >>> REPLACE ME if your backend runs somewhere other than localhost:5000
const API_BASE = "http://localhost:5000/api";

const POLL_INTERVAL_MS = 2000;
const RING_CIRCUMFERENCE = 2 * Math.PI * 34; // matches r=34 in the SVG ring

let mode = "live"; // "live" | "demo"
let pollTimer = null;
let demoTimer = null;
let demoEvents = [];

// ---------------------------------------------------------------------------
// DOM refs
// ---------------------------------------------------------------------------
const el = {
  btnLive: document.getElementById("btn-live"),
  btnDemo: document.getElementById("btn-demo"),
  statusDot: document.getElementById("status-dot"),
  deviceLabel: document.getElementById("device-label"),

  alertBanner: document.getElementById("alert-banner"),
  alertEyebrow: document.getElementById("alert-eyebrow"),
  alertTitle: document.getElementById("alert-title"),
  ringFill: document.getElementById("ring-fill"),
  confidenceValue: document.getElementById("confidence-value"),

  statusDevice: document.getElementById("status-device"),
  statusOnline: document.getElementById("status-online"),
  statusLastUpdate: document.getElementById("status-last-update"),
  statusWifi: document.getElementById("status-wifi"),

  svVibration: document.getElementById("sv-vibration"),
  sbVibration: document.getElementById("sb-vibration"),
  svAccelX: document.getElementById("sv-accelx"),
  svAccelY: document.getElementById("sv-accely"),
  svAccelZ: document.getElementById("sv-accelz"),
  svMotion: document.getElementById("sv-motion"),

  eventsBody: document.getElementById("events-body"),
  footMode: document.getElementById("foot-mode"),
};

el.ringFill.style.strokeDasharray = String(RING_CIRCUMFERENCE);
el.ringFill.style.strokeDashoffset = String(RING_CIRCUMFERENCE);

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
const LABEL_DISPLAY = {
  NORMAL: "Listening — all readings normal",
  SUSPICIOUS_ACTIVITY: "Suspicious activity detected",
  SUSPICIOUS: "Suspicious activity detected",
  POSSIBLE_TREE_CUTTING: "Possible tree-cutting activity detected",
  TREE_CUTTING: "Possible tree-cutting activity detected",
};

function stateForLabel(label) {
  const l = (label || "").toUpperCase();
  if (l.includes("TREE_CUTTING") || l === "TREE_CUTTING") return "alert";
  if (l.includes("SUSPICIOUS")) return "suspicious";
  return "normal";
}

function renderPrediction(prediction, confidence) {
  const state = stateForLabel(prediction);
  el.alertBanner.classList.remove("state-normal", "state-suspicious", "state-alert");
  el.alertBanner.classList.add(`state-${state}`);

  el.alertEyebrow.textContent =
    state === "alert" ? "Event alert" : state === "suspicious" ? "Elevated risk" : "Detection status";
  el.alertTitle.textContent = LABEL_DISPLAY[(prediction || "").toUpperCase()] || (prediction || "No data yet");

  const pct = Math.max(0, Math.min(1, confidence || 0));
  el.confidenceValue.textContent = `${Math.round(pct * 100)}%`;
  el.ringFill.style.strokeDashoffset = String(RING_CIRCUMFERENCE * (1 - pct));
}

function renderSensors(reading) {
  el.svVibration.textContent = (reading.vibration ?? 0).toFixed(2);
  el.sbVibration.style.width = `${Math.min(100, (reading.vibration ?? 0) * 100)}%`;
  el.sbVibration.style.background =
    (reading.vibration ?? 0) > 0.55 ? "var(--ember)" : (reading.vibration ?? 0) > 0.3 ? "var(--amber)" : "var(--moss)";

  el.svAccelX.innerHTML = `${(reading.accel_x ?? 0).toFixed(2)}<small>g</small>`;
  el.svAccelY.innerHTML = `${(reading.accel_y ?? 0).toFixed(2)}<small>g</small>`;
  el.svAccelZ.innerHTML = `${(reading.accel_z ?? 0).toFixed(2)}<small>g</small>`;

  const motionDetected = Number(reading.motion) === 1;
  el.svMotion.textContent = motionDetected ? "Detected" : "None";
  el.svMotion.classList.toggle("detected", motionDetected);
}

function renderStatus({ deviceId, online, lastUpdate }) {
  el.statusDevice.textContent = deviceId || "—";
  el.statusOnline.textContent = online ? "Online" : "Offline";
  el.statusOnline.className = `pill ${online ? "pill-online" : "pill-offline"}`;
  el.statusLastUpdate.textContent = lastUpdate ? new Date(lastUpdate).toLocaleTimeString() : "—";
  el.statusWifi.textContent = online ? "Connected" : "No recent signal";

  el.statusDot.classList.toggle("online", online);
  el.statusDot.classList.toggle("offline", !online);
  el.deviceLabel.textContent = `${deviceId || "ESP32_01"} — ${online ? "online" : "offline"}`;
}

function renderEvents(events) {
  if (!events || events.length === 0) {
    el.eventsBody.innerHTML = `<tr class="events-empty"><td colspan="5">No suspicious events yet — the forest is quiet.</td></tr>`;
    return;
  }

  el.eventsBody.innerHTML = events
    .slice(0, 15)
    .map((ev) => {
      const state = stateForLabel(ev.prediction);
      const tagClass = state === "alert" ? "tag-alert" : "tag-suspicious";
      const time = ev.timestamp ? new Date(ev.timestamp).toLocaleTimeString() : "—";
      const conf = ev.confidence != null ? `${Math.round(ev.confidence * 100)}%` : "—";
      return `<tr>
        <td>${time}</td>
        <td><span class="tag ${tagClass}">${(ev.prediction || "").replace(/_/g, " ")}</span></td>
        <td>${conf}</td>
        <td>${(ev.vibration ?? 0).toFixed(2)}</td>
        <td>${Number(ev.motion) === 1 ? "Yes" : "No"}</td>
      </tr>`;
    })
    .join("");
}

// ---------------------------------------------------------------------------
// LIVE MODE — polls the Flask backend
// ---------------------------------------------------------------------------
async function pollLive() {
  try {
    const [statusRes, sensorsRes, eventsRes] = await Promise.all([
      fetch(`${API_BASE}/status`),
      fetch(`${API_BASE}/sensors`),
      fetch(`${API_BASE}/events`),
    ]);

    if (!statusRes.ok || !sensorsRes.ok || !eventsRes.ok) throw new Error("Backend responded with an error");

    const status = await statusRes.json();
    const sensors = await sensorsRes.json();
    const events = await eventsRes.json();

    renderStatus({ deviceId: status.device_id, online: status.online, lastUpdate: status.last_update });
    renderSensors(sensors);
    renderPrediction(sensors.prediction, sensors.confidence);
    renderEvents(events.events);
  } catch (err) {
    // Backend unreachable — reflect that honestly rather than freezing stale data
    renderStatus({ deviceId: null, online: false, lastUpdate: null });
    console.warn("[Canopy] Could not reach backend:", err.message);
  }
}

function startLiveMode() {
  stopAllTimers();
  mode = "live";
  el.footMode.innerHTML = `Live mode — polling backend at <code id="foot-endpoint">${API_BASE}</code>`;
  pollLive();
  pollTimer = setInterval(pollLive, POLL_INTERVAL_MS);
}

// ---------------------------------------------------------------------------
// DEMO MODE — self-contained simulation, no backend required
// ---------------------------------------------------------------------------
function randomBetween(a, b) {
  return a + Math.random() * (b - a);
}

function generateDemoReading() {
  // Occasionally simulate an escalating "possible tree cutting" episode
  const roll = Math.random();
  let profile;
  if (roll < 0.12) profile = "alert";
  else if (roll < 0.25) profile = "suspicious";
  else profile = "normal";

  let reading;
  if (profile === "alert") {
    reading = {
      vibration: randomBetween(0.7, 0.97),
      accel_x: randomBetween(-0.9, 0.9),
      accel_y: randomBetween(-0.9, 0.9),
      accel_z: randomBetween(0.6, 1.4),
      motion: 1,
      prediction: "POSSIBLE_TREE_CUTTING",
      confidence: randomBetween(0.85, 0.98),
    };
  } else if (profile === "suspicious") {
    reading = {
      vibration: randomBetween(0.4, 0.65),
      accel_x: randomBetween(-0.4, 0.4),
      accel_y: randomBetween(-0.4, 0.4),
      accel_z: randomBetween(0.85, 1.15),
      motion: Math.random() > 0.4 ? 1 : 0,
      prediction: "SUSPICIOUS_ACTIVITY",
      confidence: randomBetween(0.5, 0.75),
    };
  } else {
    reading = {
      vibration: randomBetween(0.02, 0.2),
      accel_x: randomBetween(-0.08, 0.08),
      accel_y: randomBetween(-0.08, 0.08),
      accel_z: randomBetween(0.95, 1.05),
      motion: Math.random() > 0.85 ? 1 : 0,
      prediction: "NORMAL",
      confidence: randomBetween(0.85, 0.98),
    };
  }

  reading.device_id = "ESP32_01 (demo)";
  reading.timestamp = new Date().toISOString();
  return reading;
}

function tickDemo() {
  const reading = generateDemoReading();

  renderStatus({ deviceId: reading.device_id, online: true, lastUpdate: reading.timestamp });
  renderSensors(reading);
  renderPrediction(reading.prediction, reading.confidence);

  if (reading.prediction !== "NORMAL") {
    demoEvents.unshift(reading);
    demoEvents = demoEvents.slice(0, 15);
    renderEvents(demoEvents);
  }
}

function startDemoMode() {
  stopAllTimers();
  mode = "demo";
  demoEvents = [];
  el.footMode.textContent = "Demo mode — simulated sensor data, no ESP32 or backend needed";
  renderEvents([]);
  tickDemo();
  demoTimer = setInterval(tickDemo, POLL_INTERVAL_MS);
}

function stopAllTimers() {
  if (pollTimer) clearInterval(pollTimer);
  if (demoTimer) clearInterval(demoTimer);
  pollTimer = null;
  demoTimer = null;
}

// ---------------------------------------------------------------------------
// Mode toggle wiring
// ---------------------------------------------------------------------------
el.btnLive.addEventListener("click", () => {
  el.btnLive.classList.add("active");
  el.btnDemo.classList.remove("active");
  startLiveMode();
});

el.btnDemo.addEventListener("click", () => {
  el.btnDemo.classList.add("active");
  el.btnLive.classList.remove("active");
  startDemoMode();
});

// Start in live mode by default; falls back gracefully to "offline" display
// if no backend is running yet.
startLiveMode();
