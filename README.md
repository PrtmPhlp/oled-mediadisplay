# OLED Media Display

Python script to display Home Assistant media player status on an SH1106 OLED display with a starfield animation.

- Displays current media title from Home Assistant.
- Auto-scrolls long titles.
- Starfield animation when idle or below the title.
- Controlled via Home Assistant helper to enable/disable the display


Edit `.env` and fill in your Home Assistant details:
  - `HA_BASE_URL`: URL to your Home Assistant instance
	- `HA_TOKEN`: Long-lived access token.
  - `HA_MEDIA_PLAYER`: Entity ID of the media player.
	- `HA_DISPLAY_HELPER`: Entity ID of the input_boolean to control the display.

To run this script continuously, you can deploy it as a systemd service using the provided `oled-mediaplayer.service` file.