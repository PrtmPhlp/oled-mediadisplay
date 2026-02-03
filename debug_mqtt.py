#!/usr/bin/env python3
"""Debug: List ALL MQTT topics under iotstack/shairport."""

import os
import paho.mqtt.client as mqtt

# Load from environment variables
MQTT_BROKER = os.getenv("MQTT_BROKER", "localhost")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
MQTT_USER = os.getenv("MQTT_USER", "")
MQTT_PASS = os.getenv("MQTT_PASS", "")
MQTT_TOPIC_BASE = os.getenv("MQTT_TOPIC_BASE", "iotstack/shairport")


def on_connect(client, userdata, flags, reason_code, properties):
    print(f"Connected: {reason_code}")
    # Subscribe to ALL topics under the base
    client.subscribe(f"{MQTT_TOPIC_BASE}/#")
    print(f"Subscribed to {MQTT_TOPIC_BASE}/#")


def on_message(client, userdata, msg):
    payload = msg.payload
    # Try to decode, show raw bytes if binary
    try:
        payload_str = payload.decode("utf-8")[:100]
    except:
        payload_str = f"[binary: {len(payload)} bytes] {payload[:50]}"

    print(f"TOPIC: {msg.topic}")
    print(f"  PAYLOAD: {payload_str}")
    print()


def main():
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    if MQTT_USER:
        client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.on_connect = on_connect
    client.on_message = on_message

    client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)

    print("Listening for ALL shairport topics... (Ctrl+C to stop)")
    print("Play some music to see what topics are published.\n")

    try:
        client.loop_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
