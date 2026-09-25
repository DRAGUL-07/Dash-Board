# Canopy — IoT Forest Monitoring for Illegal Tree-Cutting Detection

An ESP32-based system that senses vibration, motion, and IMU activity near
trees, classifies it as `NORMAL` / `SUSPICIOUS_ACTIVITY` / `POSSIBLE_TREE_CUTTING`,
and reports results to a live dashboard over Wi-Fi. Built for a college
hackathon demo — lightweight, free/open-source, no cloud dependency.

```
forest-monitoring/
│
├── esp32/
│   ├── forest_monitor.ino     # ESP32 firmware (sensors + WiFi + inference)
│   └── model/
│       └── README.md          # How to convert & wire in YOUR trained model
│
├── backend/
│   ├── app.py                 # Flask REST API
│   ├── requirements.txt
│   └── database/              # SQLite event log (created automatically)
│
├── dashboard/
│   ├── index.html
│   ├── style.css
│   └── script.js              # Polls backend; also has a self-contained Demo mode
│
└── README.md
```

---

## What YOU need to fill in before this runs for real

Everything else is complete, runnable code. These are the only things that
are genuinely yours to provide (all marked `>>> REPLACE ME` in the files):

| What | Where |
|---|---|
| Wi-Fi SSID / password | `esp32/forest_monitor.ino` (top of file) |
| Backend server URL (your PC's LAN IP) | `esp32/forest_monitor.ino` |
| GPIO pins for vibration / PIR / IMU I2C | `esp32/forest_monitor.ino` |
| Your trained model, converted to `model.h` | `esp32/model/` — see `esp32/model/README.md` |
| Model input feature count, output class names, normalization values | `esp32/forest_monitor.ino`, `MODEL_*` section |

Until the model is wired in, the firmware runs in **heuristic/demo mode**
(`USE_TFLITE_MODEL 0`) — it uses simple vibration/motion thresholds so you
can test the entire pipeline end-to-end immediately.

---

## Data flow

```
[Vibration + IMU + PIR sensors]
        |  (raw analog/digital/I2C reads, every 200ms)
        v
[ESP32: filtering / EMA smoothing]
        |  (build feature vector, normalize)
        v
[ESP32: ML inference — TFLite Micro, or heuristic fallback]
        |  (label + confidence, every 2s)
        v
[Wi-Fi: HTTP POST /api/data]  --->  [Flask backend]
        |                                |
        |                       stores latest reading + SQLite log
        |                                |
        v                                v
                            [Dashboard polls /api/status, /api/sensors, /api/events]
                                          |
                                          v
                            [Live dashboard: status, sensors, ML result, events]
```

---

## 1. ESP32 setup

**Board:** Any standard ESP32 dev board (>>> confirm your exact board variant
in Arduino IDE's Board Manager — e.g. "ESP32 Dev Module", "DOIT ESP32 DEVKIT V1").

**Wiring (defaults used in the code — change pins as needed):**
- Vibration sensor → GPIO 34 (analog) or your digital pin
- PIR motion sensor → GPIO 27
- IMU (MPU6050-style) → I2C: SDA = GPIO 21, SCL = GPIO 22

**Arduino IDE setup:**
1. Install the ESP32 board package: **File → Preferences → Additional
   Board URLs** → add `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`,
   then **Tools → Board → Boards Manager** → install "esp32".
2. Select your board under **Tools → Board**.
3. Open `esp32/forest_monitor.ino`.
4. Fill in the `>>> REPLACE ME` fields (Wi-Fi, server URL, pins).
5. Click Upload.
6. Open **Tools → Serial Monitor** at 115200 baud to watch debug output.

No extra libraries are required for demo/heuristic mode (`WiFi.h`,
`HTTPClient.h`, `Wire.h` all ship with the ESP32 board package). For real
ML inference, install **TensorFlowLite_ESP32** via Library Manager — see
`esp32/model/README.md`.

---

## 2. Backend setup

```bash
cd backend
pip install -r requirements.txt
python app.py
```

Server starts at `http://0.0.0.0:5000`. Find your machine's LAN IP
(`ipconfig` on Windows, `ifconfig`/`ip a` on Mac/Linux) and put it into
`SERVER_URL` in the ESP32 sketch — the ESP32 cannot reach `localhost`,
that refers to itself.

**Endpoints:**
- `POST /api/data` — ESP32 posts sensor + prediction data here
- `GET /api/status` — device online/offline + last update time
- `GET /api/sensors` — latest raw sensor values + current prediction
- `GET /api/events` — recent `SUSPICIOUS_ACTIVITY` / `POSSIBLE_TREE_CUTTING` events
- `GET /api/health` — simple health check

Data is kept in memory for speed and also logged to
`backend/database/events.db` (SQLite) so history survives a restart.

---

## 3. Dashboard setup

The dashboard is static HTML/CSS/JS — no build step. Two ways to run it:

**Option A — just open it:**
Double-click `dashboard/index.html` (or open it in a browser). It will try
to poll the backend at `http://localhost:5000/api` (edit `API_BASE` in
`script.js` if your backend is on a different host/port).

**Option B — serve it (avoids some browsers' local-file restrictions):**
```bash
cd dashboard
python -m http.server 8080
```
Then visit `http://localhost:8080`.

**Demo mode:** click the **Demo** toggle in the top bar to switch to fully
simulated data — no ESP32 or backend needed. Use this if your hardware
isn't available during the presentation. Click **Live** to go back to
polling the real backend.

---

## 4. Full startup order for a live demo

1. `cd backend && python app.py`
2. Power on / reset the ESP32 (make sure it's on the same Wi-Fi network as
   your laptop, and `SERVER_URL` points at your laptop's current IP).
3. Open `dashboard/index.html` (or serve it as above), make sure **Live** is
   selected.
4. Watch the Serial Monitor to confirm the ESP32 is posting data
   (`[Report] HTTP 200: ...`), and watch the dashboard update every ~2s.
5. If hardware misbehaves mid-demo, switch the dashboard to **Demo** mode
   without missing a beat.

---

## Troubleshooting

**ESP32 won't connect to Wi-Fi**
- Confirm SSID/password are correct and the network is 2.4GHz (ESP32 does
  not support 5GHz Wi-Fi).
- Check Serial Monitor for `[WiFi] Failed to connect within timeout` — the
  firmware will keep retrying automatically in `loop()`.

**ESP32 connects to Wi-Fi but POST fails / dashboard shows "Offline"**
- Make sure `SERVER_URL` uses your laptop's actual LAN IP, not `localhost`
  or `127.0.0.1`.
- Make sure your laptop's firewall allows inbound connections on port 5000.
- Confirm phone/laptop and ESP32 are on the *same* Wi-Fi network (not a
  guest network that isolates clients from each other).
- Check the backend terminal — you should see `[/api/data] ESP32_01 -> ...`
  logged for every successful POST.

**Dashboard shows stale/offline even though ESP32 is posting**
- `OFFLINE_THRESHOLD_SECONDS` in `backend/app.py` marks a device offline
  after 15s of silence — check the ESP32's `REPORT_INTERVAL_MS` (default
  2000ms) isn't being starved by a slow Wi-Fi reconnect loop.
- Open browser dev tools (F12) → Console/Network tab to see if `fetch()`
  calls to `/api/status` etc. are failing (often a CORS or wrong-IP issue).

**TFLite Micro: `AllocateTensors() failed`**
- Increase `kTensorArenaSize` in `forest_monitor.ino` (start at 20KB, try
  doubling it).

**TFLite Micro: predictions look wrong/random**
- Almost always a preprocessing mismatch. Double check `NORM_PARAMS[]` and
  `buildFeatureVector()` exactly mirror what your training script did
  (same feature order, same normalization, same units).

**IMU readings are all zero / `readIMU()` returns false**
- Check wiring (SDA/SCL swapped is the most common mistake) and confirm
  `IMU_I2C_ADDR` (0x68 vs 0x69, depends on the AD0 pin).

---

## Notes on the ML model

This project deliberately does **not** invent your model's input shape,
output classes, or preprocessing — see `esp32/model/README.md` for the
full conversion guide and exactly which placeholders to fill in with your
real model's details. Until then, `USE_TFLITE_MODEL 0` runs a transparent
threshold-based stand-in so the rest of the pipeline (sensors → Wi-Fi →
backend → dashboard) can be fully tested and demoed on its own.
