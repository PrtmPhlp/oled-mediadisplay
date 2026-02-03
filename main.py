#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import time
import threading
from random import randrange
from pathlib import Path
from typing import Optional, Any
from PIL import Image, ImageDraw, ImageFont
from luma.core.interface.serial import i2c
from luma.oled.device import sh1106
from luma.core.render import canvas
import paho.mqtt.client as mqtt

# config
I2C_PORT = 1
I2C_ADDR = 0x3C
WIDTH, HEIGHT = 128, 64
ROTATE = 2

FONT_FILENAME = "fonts/PixelOperator.ttf"
FONT_SIZE = 16

# MQTT Config (load from environment)
MQTT_BROKER = os.getenv("MQTT_BROKER", "localhost")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
MQTT_USER = os.getenv("MQTT_USER", "")
MQTT_PASS = os.getenv("MQTT_PASS", "")
MQTT_TOPIC_BASE = os.getenv("MQTT_TOPIC_BASE", "iotstack/shairport")

FPS = 30
SCROLL_SPEED = 2
SCROLL_DELAY = 2.0  # wait before scrollen (in s)
GAP_PX = 32

STAR_NUM = 512
STAR_MAX_DEPTH = 32
STAR_Z_STEP = 0.2
STAR_VIEW_Y_OFFSET = 15  # Height of title bar


# Helpers
def get_font(path: Path, size: int) -> Any:
    try:
        return ImageFont.truetype(str(path), size)
    except OSError:
        return ImageFont.load_default()


def get_text_width(font: Any, text: str) -> int:
    # Create a dummy image to use textbbox
    tmp = Image.new("1", (1, 1))
    d = ImageDraw.Draw(tmp)
    bbox = d.textbbox((0, 0), text, font=font)
    return int(bbox[2] - bbox[0])


class ShairportMQTTClient:
    """MQTT client for receiving shairport-sync metadata."""

    def __init__(self, broker: str, port: int, username: str, password: str, topic_base: str):
        self.broker = broker
        self.port = port
        self.topic_base = topic_base

        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        self.client.username_pw_set(username, password)
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.on_disconnect = self._on_disconnect

        # Thread-safe data storage
        self._lock = threading.Lock()
        self._title: Optional[str] = None
        self._artist: Optional[str] = None
        self._is_active: bool = False
        self._connected: bool = False

    def _on_connect(self, client, userdata, flags, reason_code, properties):
        print(f"MQTT connected with result code {reason_code}")
        self._connected = True
        # Subscribe to relevant topics
        topics = [
            f"{self.topic_base}/title",
            f"{self.topic_base}/artist",
            f"{self.topic_base}/active_start",
            f"{self.topic_base}/active_end",
            f"{self.topic_base}/play_start",
            f"{self.topic_base}/play_end",
        ]
        for topic in topics:
            client.subscribe(topic)
            print(f"Subscribed to {topic}")

    def _on_disconnect(self, client, userdata, flags, reason_code, properties):
        print(f"MQTT disconnected with result code {reason_code}")
        self._connected = False

    def _on_message(self, client, userdata, msg):
        topic = msg.topic
        payload = msg.payload.decode("utf-8", errors="ignore").strip()

        print(f"MQTT: {topic} = {payload[:50] if payload else '(empty)'}")

        with self._lock:
            if topic == f"{self.topic_base}/title":
                self._title = payload if payload and payload != "--" else None
            elif topic == f"{self.topic_base}/artist":
                self._artist = payload if payload and payload != "--" else None
            elif topic == f"{self.topic_base}/active_start":
                self._is_active = True
            elif topic == f"{self.topic_base}/active_end":
                self._is_active = False
                self._title = None
                self._artist = None
            elif topic == f"{self.topic_base}/play_end":
                # Song ended, clear title (but session may still be active)
                self._title = None
                self._artist = None

    def start(self):
        """Start the MQTT client in a background thread."""
        try:
            self.client.connect(self.broker, self.port, keepalive=60)
            self.client.loop_start()
        except Exception as e:
            print(f"MQTT connection error: {e}")

    def stop(self):
        """Stop the MQTT client."""
        self.client.loop_stop()
        self.client.disconnect()

    def is_active(self) -> bool:
        """Check if there's an active AirPlay session."""
        with self._lock:
            return self._is_active

    def get_display_title(self) -> Optional[str]:
        """Get formatted title (with artist if available)."""
        with self._lock:
            if not self._title:
                return None
            if self._artist:
                return f"{self._artist} - {self._title}"
            return self._title


class Starfield:
    def __init__(self, width: int, height: int, num_stars: int, max_depth: int):
        self.width = width
        self.height = height
        self.num_stars = num_stars
        self.max_depth = max_depth
        self.stars = []
        self.init_stars()

        # Viewport defaults to full screen
        self.vp_x = 0
        self.vp_y = 0
        self.vp_w = width
        self.vp_h = height
        self.origin_x = width // 2
        self.origin_y = height // 2

    def init_stars(self):
        self.stars = [
            [float(randrange(-25, 25)), float(randrange(-25, 25)), float(randrange(1, self.max_depth))]
            for _ in range(self.num_stars)
        ]

    def set_viewport(self, x: int, y: int, w: int, h: int):
        self.vp_x = x
        self.vp_y = y
        self.vp_w = w
        self.vp_h = h
        # Center origin in the new viewport
        self.origin_x = x + w // 2
        self.origin_y = y + h // 2

    def update_and_draw(self, draw: ImageDraw.ImageDraw):
        for star in self.stars:
            star[2] -= STAR_Z_STEP
            if star[2] <= 0:
                star[0] = float(randrange(-25, 25))
                star[1] = float(randrange(-25, 25))
                star[2] = float(self.max_depth)

            k = 128.0 / star[2]
            x = int(star[0] * k + self.origin_x)
            y = int(star[1] * k + self.origin_y)

            if (self.vp_x <= x < self.vp_x + self.vp_w) and (self.vp_y <= y < self.vp_y + self.vp_h):
                draw.point((x, y), fill=255)
                # Make near stars slightly bigger
                if (1 - star[2] / self.max_depth) * 4 >= 2:
                    draw.point((x + 1, y), fill=255)


class TitleDisplay:
    def __init__(self, width: int, font: Any):
        self.width = width
        self.font = font
        self.text = None
        self.display_text = ""
        self.is_scrolling = False
        self.scroll_offset = 0.0
        self.text_width = 0
        self.scroll_wait_start = 0.0
        self.waiting_to_scroll = False

    def set_title(self, title: str):
        if title == self.text:
            return

        self.text = title
        self.scroll_offset = 0.0
        self.waiting_to_scroll = True
        self.scroll_wait_start = time.time()

        # Determine display format
        base_w = get_text_width(self.font, title)
        dashed = f"- {title} -"
        dashed_w = get_text_width(self.font, dashed)

        if dashed_w <= self.width:
            self.display_text = dashed
            self.text_width = dashed_w
            self.is_scrolling = False
        elif base_w <= self.width:
            self.display_text = title
            self.text_width = base_w
            self.is_scrolling = False
        else:
            self.display_text = title + "   "  # Add gap for scrolling
            self.text_width = get_text_width(self.font, self.display_text)
            self.is_scrolling = True

    def update(self):
        if not self.is_scrolling:
            return

        now = time.time()
        if self.waiting_to_scroll:
            if now - self.scroll_wait_start >= SCROLL_DELAY:
                self.waiting_to_scroll = False
            return

        self.scroll_offset += SCROLL_SPEED
        if self.scroll_offset >= self.text_width:
            self.scroll_offset = 0
            self.waiting_to_scroll = True
            self.scroll_wait_start = time.time()

    def draw(self, draw: ImageDraw.ImageDraw, y: int):
        if not self.is_scrolling:
            # Center
            x = (self.width - self.text_width) // 2
            draw.text((x, y), self.display_text, font=self.font, fill=255)
        else:
            # Scroll
            x = -int(self.scroll_offset)
            draw.text((x, y), self.display_text, font=self.font, fill=255)
            draw.text((x + self.text_width, y), self.display_text, font=self.font, fill=255)


def main():
    # Setup Device
    serial = i2c(port=I2C_PORT, address=I2C_ADDR)
    device = sh1106(serial, width=WIDTH, height=HEIGHT, rotate=ROTATE)
    device.contrast(255)

    # Setup Components
    font_path = Path(__file__).parent / FONT_FILENAME
    font = get_font(font_path, FONT_SIZE)

    mqtt_client = ShairportMQTTClient(
        broker=MQTT_BROKER,
        port=MQTT_PORT,
        username=MQTT_USER,
        password=MQTT_PASS,
        topic_base=MQTT_TOPIC_BASE
    )
    mqtt_client.start()

    starfield = Starfield(WIDTH, HEIGHT, STAR_NUM, STAR_MAX_DEPTH)
    title_display = TitleDisplay(WIDTH, font)

    title = None

    try:
        while True:
            start_time = time.time()

            # 1. Get current state from MQTT (no polling needed - data is pushed)
            title = mqtt_client.get_display_title()

            # 2. Update State
            if title:
                title_display.set_title(title)
                title_display.update()
                # Partial Starfield (below title)
                starfield.set_viewport(0, STAR_VIEW_Y_OFFSET, WIDTH, HEIGHT - STAR_VIEW_Y_OFFSET)
            else:
                # Full Starfield
                starfield.set_viewport(0, 0, WIDTH, HEIGHT)

            # 3. Draw
            with canvas(device) as draw:
                if title:
                    title_display.draw(draw, y=0)

                starfield.update_and_draw(draw)

            # 4. FPS Control
            elapsed = time.time() - start_time
            sleep_time = (1.0 / FPS) - elapsed
            if sleep_time > 0:
                time.sleep(sleep_time)

    except KeyboardInterrupt:
        pass
    finally:
        mqtt_client.stop()


if __name__ == "__main__":
    main()
