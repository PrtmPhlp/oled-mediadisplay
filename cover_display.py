#!/usr/bin/env python3
"""Test: Display album cover from MQTT on OLED."""

import io
import time
import threading
from typing import Optional
from PIL import Image
from luma.core.interface.serial import i2c
from luma.oled.device import sh1106
from luma.core.render import canvas
import paho.mqtt.client as mqtt

# Config
I2C_PORT = 1
I2C_ADDR = 0x3C
WIDTH, HEIGHT = 128, 64
ROTATE = 2

MQTT_BROKER = "10.0.0.50"
MQTT_PORT = 1883
MQTT_USER = "mqtt"
MQTT_PASS = "Mo1-rspb-SoSe"
MQTT_TOPIC_BASE = "iotstack/shairport"

# Cover size options - try different sizes!
COVER_SIZE = 48  # 32, 48, or 64

# Display timeout
DISPLAY_TIMEOUT = 5 * 60  # 5 minutes in seconds


class CoverTest:
    def __init__(self):
        self.cover_image: Optional[Image.Image] = None
        self.title: Optional[str] = None
        self.artist: Optional[str] = None
        self.last_activity: float = time.time()  # Track last playback activity
        self._lock = threading.Lock()

        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        self.client.username_pw_set(MQTT_USER, MQTT_PASS)
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message

    def _on_connect(self, client, userdata, flags, reason_code, properties):
        print(f"Connected: {reason_code}")
        client.subscribe(f"{MQTT_TOPIC_BASE}/cover")
        client.subscribe(f"{MQTT_TOPIC_BASE}/title")
        client.subscribe(f"{MQTT_TOPIC_BASE}/artist")
        client.subscribe(f"{MQTT_TOPIC_BASE}/play_start")
        client.subscribe(f"{MQTT_TOPIC_BASE}/play_end")
        client.subscribe(f"{MQTT_TOPIC_BASE}/play_resume")
        print("Subscribed to cover, title, artist, play events")

    def _on_message(self, client, userdata, msg):
        topic = msg.topic

        if topic == f"{MQTT_TOPIC_BASE}/cover":
            print(f"Received cover: {len(msg.payload)} bytes")
            try:
                # Load JPEG from binary data
                img = Image.open(io.BytesIO(msg.payload))
                print(f"  Original size: {img.size}, mode: {img.mode}")

                # Convert to grayscale
                img = img.convert("L")

                # Resize to square (crop center if needed)
                min_dim = min(img.size)
                left = (img.width - min_dim) // 2
                top = (img.height - min_dim) // 2
                img = img.crop((left, top, left + min_dim, top + min_dim))

                # Resize to target size
                img = img.resize((COVER_SIZE, COVER_SIZE), Image.Resampling.LANCZOS)

                # Convert to 1-bit with dithering (Floyd-Steinberg)
                img = img.convert("1")  # Uses dithering by default

                print(f"  Processed: {img.size}, mode: {img.mode}")

                with self._lock:
                    self.cover_image = img

            except Exception as e:
                print(f"  Error processing cover: {e}")

        elif topic == f"{MQTT_TOPIC_BASE}/title":
            with self._lock:
                self.title = msg.payload.decode("utf-8", errors="ignore").strip()
            print(f"Title: {self.title}")

        elif topic == f"{MQTT_TOPIC_BASE}/artist":
            with self._lock:
                self.artist = msg.payload.decode("utf-8", errors="ignore").strip()
                self.last_activity = time.time()
            print(f"Artist: {self.artist}")

        elif topic in [f"{MQTT_TOPIC_BASE}/play_start", f"{MQTT_TOPIC_BASE}/play_resume"]:
            with self._lock:
                self.last_activity = time.time()
            print("Playback started/resumed")

        elif topic == f"{MQTT_TOPIC_BASE}/play_end":
            print("Playback ended")

    def start(self):
        self.client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
        self.client.loop_start()

    def stop(self):
        self.client.loop_stop()
        self.client.disconnect()

    def get_cover(self) -> Optional[Image.Image]:
        with self._lock:
            return self.cover_image

    def get_info(self) -> tuple[Optional[str], Optional[str]]:
        with self._lock:
            return self.artist, self.title

    def is_timed_out(self) -> bool:
        """Check if display should be turned off due to inactivity."""
        with self._lock:
            return (time.time() - self.last_activity) > DISPLAY_TIMEOUT

    def reset_activity(self):
        """Reset the activity timer."""
        with self._lock:
            self.last_activity = time.time()


def main():
    # Setup Display
    serial = i2c(port=I2C_PORT, address=I2C_ADDR)
    device = sh1106(serial, width=WIDTH, height=HEIGHT, rotate=ROTATE)
    device.contrast(255)

    cover_test = CoverTest()
    cover_test.start()

    print(f"\nWaiting for cover art... (Cover size: {COVER_SIZE}x{COVER_SIZE})")
    print("Play a song to see the cover!\n")

    last_cover = None
    last_artist = None
    last_title = None
    display_is_off = False

    try:
        while True:
            cover = cover_test.get_cover()
            artist, title = cover_test.get_info()
            timed_out = cover_test.is_timed_out()

            # Handle display timeout
            if timed_out and not display_is_off:
                device.hide()
                display_is_off = True
                print("Display turned off (timeout)")
            elif not timed_out and display_is_off:
                device.show()
                display_is_off = False
                print("Display turned on")

            # Only redraw when something changed and display is on
            if not display_is_off and (cover != last_cover or artist != last_artist or title != last_title):
                last_cover = cover
                last_artist = artist
                last_title = title

                if cover:
                    # Create and display the layout directly (not inside canvas)
                    img = create_layout_left(cover, artist, title)
                    device.display(img)
                else:
                    with canvas(device) as draw:
                        draw.text((10, HEIGHT//2 - 5), "Waiting for cover...", fill=255)

            time.sleep(0.1)

    except KeyboardInterrupt:
        pass
    finally:
        cover_test.stop()


def create_layout_left(cover: Image.Image, artist: Optional[str], title: Optional[str]) -> Image.Image:
    """Create layout with cover on left, text on right."""
    from PIL import ImageDraw, ImageFont

    # Create full-screen image
    img = Image.new("1", (WIDTH, HEIGHT), 0)
    draw = ImageDraw.Draw(img)

    # Paste cover on left
    img.paste(cover, (0, (HEIGHT - COVER_SIZE) // 2))

    # Text area starts after cover
    text_x = COVER_SIZE + 4
    text_width = WIDTH - text_x

    # Try to load font, fallback to default
    try:
        from pathlib import Path
        font_path = Path(__file__).parent / "fonts/PixelOperator.ttf"
        font_small = ImageFont.truetype(str(font_path), 12)
        font_large = ImageFont.truetype(str(font_path), 14)
    except:
        font_small = ImageFont.load_default()
        font_large = font_small

    # Draw artist (smaller, top)
    if artist:
        # Truncate if too long
        display_artist = artist[:12] + "..." if len(artist) > 15 else artist
        draw.text((text_x, 8), display_artist, font=font_small, fill=255)

    # Draw title (larger, bottom)
    if title:
        display_title = title[:12] + "..." if len(title) > 15 else title
        draw.text((text_x, 28), display_title, font=font_large, fill=255)

    return img


if __name__ == "__main__":
    main()
