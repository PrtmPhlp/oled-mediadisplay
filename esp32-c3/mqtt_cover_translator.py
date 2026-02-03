#!/usr/bin/env python3
"""MQTT shairport forwarder with retain=True and cover conversion."""

import argparse
import io
import os
import sys
from typing import Optional

from PIL import Image
import paho.mqtt.client as mqtt


# Load from environment variables with sensible defaults
DEFAULT_BROKER = os.getenv("MQTT_BROKER", "localhost")
DEFAULT_PORT = int(os.getenv("MQTT_PORT", "1883"))
DEFAULT_USER = os.getenv("MQTT_USER", "")
DEFAULT_PASS = os.getenv("MQTT_PASS", "")
DEFAULT_TOPIC_IN = os.getenv("MQTT_TOPIC_IN", "iotstack/shairport/#")
DEFAULT_TOPIC_OUT = os.getenv("MQTT_TOPIC_OUT", "iotstack/shairport-extension")
DEFAULT_SIZE = 48


class CoverTranslator:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        if args.user:
            self.client.username_pw_set(args.user, args.password)
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message

    def _on_connect(self, client, userdata, flags, reason_code, properties):
        print(f"Connected: {reason_code}")
        client.subscribe(self.args.topic_in)
        print(f"Subscribed to {self.args.topic_in}")

    def _on_message(self, client, userdata, msg):
        # Extract subtopic relative to iotstack/shairport/
        topic_in_base = self.args.topic_in.rstrip('/#')
        if not msg.topic.startswith(topic_in_base + "/"):
            return

        subtopic = msg.topic[len(topic_in_base) + 1:]
        new_topic = f"{self.args.topic_out}/{subtopic}"

        # Special handling for cover: convert to bitmap
        if subtopic == "cover":
            payload = msg.payload
            if payload == b"--":
                client.publish(new_topic, payload, retain=True)
                print(f"Published empty cover to {new_topic}")
                return

            try:
                bitmap = self._convert_cover(payload)
                client.publish(new_topic, bitmap, retain=True)
                print(f"Published {len(bitmap)} bytes (converted cover) to {new_topic}")
            except Exception as exc:
                print(f"Convert error: {exc}")
            return

        # For all other topics: forward as-is with retain=True
        client.publish(new_topic, msg.payload, retain=True)
        print(f"Forwarded to {new_topic} (retain=True)")

    def _convert_cover(self, payload: bytes) -> bytes:
        img = Image.open(io.BytesIO(payload))
        img = img.convert("L")

        # center-crop to square like test_cover.py
        min_dim = min(img.size)
        left = (img.width - min_dim) // 2
        top = (img.height - min_dim) // 2
        img = img.crop((left, top, left + min_dim, top + min_dim))

        img = img.resize((self.args.size, self.args.size), Image.Resampling.LANCZOS)
        img = img.convert("1")  # Floyd-Steinberg dithering by default

        # Pack to XBM (LSB first) rows
        pixels = img.load()
        width, height = img.size
        out = bytearray((width * height + 7) // 8)
        for y in range(height):
            for x in range(width):
                if pixels[x, y] != 0:
                    idx = y * width + x
                    out[idx >> 3] |= 1 << (idx & 7)
        return bytes(out)

    def run(self) -> None:
        self.client.connect(self.args.host, self.args.port, keepalive=60)
        self.client.loop_forever()


def parse_args(argv: Optional[list[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="MQTT shairport forwarder with retain=True")
    parser.add_argument("--host", default=DEFAULT_BROKER)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--user", default=DEFAULT_USER)
    parser.add_argument("--password", default=DEFAULT_PASS)
    parser.add_argument("--topic-in", default=DEFAULT_TOPIC_IN)
    parser.add_argument("--topic-out", default=DEFAULT_TOPIC_OUT)
    parser.add_argument("--size", type=int, default=DEFAULT_SIZE)
    return parser.parse_args(argv)


def main() -> int:
    args = parse_args()
    translator = CoverTranslator(args)
    translator.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
