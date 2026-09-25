"""
Forest Monitoring System — Backend API
---------------------------------------
Lightweight Flask server for a hackathon demo. Receives sensor + ML
detection data from the ESP32, stores it in-memory (+ optional SQLite log),
and exposes REST endpoints the dashboard polls.

Run:
    pip install -r requirements.txt
    python app.py

Server starts at http://0.0.0.0:5000
"""

import json
import sqlite3
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

from flask import Flask, jsonify, request
from flask_cors import CORS

app = Flask(__name__)
CORS(app)  # allow the dashboard (served separately) to call this API

DB_PATH = Path(__file__).parent / "database" / "events.db"
DB_PATH.parent.mkdir(exist_ok=True)

# How long (seconds) with no data before a device is considered "offline"
OFFLINE_THRESHOLD_SECONDS = 15

# ---------------------------------------------------------------------------
# In-memory state (simple + fast, good enough for a demo)
# ---------------------------------------------------------------------------
state_lock = threading.Lock()
latest_reading = {
    "device_id": None,
    "timestamp": None,
    "vibration": 0,
    "accel_x": 0,
    "accel_y": 0,
    "accel_z": 0,
    "motion": 0,
    "prediction": "NORMAL",
    "confidence": 0.0,
    "received_at": None,   # server-side receive time, used for online/offline
}
recent_events = []          # rolling list of interesting events (SUSPICIOUS / POSSIBLE_TREE_CUTTING)
MAX_RECENT_EVENTS = 100

ALERT_LABELS = {"SUSPICIOUS_ACTIVITY", "SUSPICIOUS", "POSSIBLE_TREE_CUTTING", "TREE_CUTTING"}


# ---------------------------------------------------------------------------
# SQLite persistence (so events survive a server restart)
# ---------------------------------------------------------------------------
def init_db():
    conn = sqlite3.connect(DB_PATH)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id TEXT,
            timestamp TEXT,
            vibration REAL,
            accel_x REAL,
            accel_y REAL,
            accel_z REAL,
            motion INTEGER,
            prediction TEXT,
            confidence REAL,
            received_at TEXT
        )
    """)
    conn.commit()
    conn.close()


def save_event_to_db(reading: dict):
    conn = sqlite3.connect(DB_PATH)
    conn.execute(
        """INSERT INTO events
           (device_id, timestamp, vibration, accel_x, accel_y, accel_z,
            motion, prediction, confidence, received_at)
           VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)""",
        (
            reading.get("device_id"), reading.get("timestamp"),
            reading.get("vibration"), reading.get("accel_x"),
            reading.get("accel_y"), reading.get("accel_z"),
            reading.get("motion"), reading.get("prediction"),
            reading.get("confidence"), reading.get("received_at"),
        ),
    )
    conn.commit()
    conn.close()


# ---------------------------------------------------------------------------
# Routes
# ---------------------------------------------------------------------------
@app.route("/api/data", methods=["POST"])
def receive_data():
    """ESP32 posts sensor + prediction data here."""
    payload = request.get_json(silent=True)
    if not payload:
        return jsonify({"error": "Invalid or missing JSON body"}), 400

    required_fields = ["device_id", "prediction"]
    missing = [f for f in required_fields if f not in payload]
    if missing:
        return jsonify({"error": f"Missing fields: {missing}"}), 400

    now_iso = datetime.now(timezone.utc).isoformat()
    reading = {
        "device_id": payload.get("device_id"),
        "timestamp": payload.get("timestamp", now_iso),
        "vibration": payload.get("vibration", 0),
        "accel_x": payload.get("accel_x", 0),
        "accel_y": payload.get("accel_y", 0),
        "accel_z": payload.get("accel_z", 0),
        "motion": payload.get("motion", 0),
        "prediction": payload.get("prediction", "UNKNOWN"),
        "confidence": payload.get("confidence", 0.0),
        "received_at": now_iso,
    }

    with state_lock:
        latest_reading.update(reading)

        is_alert = reading["prediction"].upper() in ALERT_LABELS
        if is_alert:
            recent_events.insert(0, reading)
            del recent_events[MAX_RECENT_EVENTS:]

    save_event_to_db(reading)

    print(f"[/api/data] {reading['device_id']} -> {reading['prediction']} "
          f"({reading['confidence']:.2f})")

    return jsonify({"status": "ok", "received": reading}), 200


@app.route("/api/status", methods=["GET"])
def get_status():
    """System / device online status."""
    with state_lock:
        received_at = latest_reading.get("received_at")
        online = False
        seconds_since_last = None
        if received_at:
            last_dt = datetime.fromisoformat(received_at)
            seconds_since_last = (datetime.now(timezone.utc) - last_dt).total_seconds()
            online = seconds_since_last < OFFLINE_THRESHOLD_SECONDS

        return jsonify({
            "device_id": latest_reading.get("device_id"),
            "online": online,
            "last_update": received_at,
            "seconds_since_last_update": seconds_since_last,
        })


@app.route("/api/sensors", methods=["GET"])
def get_sensors():
    """Latest raw sensor values + current prediction."""
    with state_lock:
        return jsonify(dict(latest_reading))


@app.route("/api/events", methods=["GET"])
def get_events():
    """Recent suspicious / tree-cutting events."""
    limit = request.args.get("limit", default=20, type=int)
    with state_lock:
        return jsonify({"events": recent_events[:limit]})


@app.route("/api/health", methods=["GET"])
def health_check():
    return jsonify({"status": "backend running"}), 200


if __name__ == "__main__":
    init_db()
    print("=== Forest Monitoring Backend ===")
    print("POST sensor data to:  http://<this-machine-ip>:5000/api/data")
    print("Dashboard should poll: /api/status, /api/sensors, /api/events")
    app.run(host="0.0.0.0", port=5000, debug=True)
