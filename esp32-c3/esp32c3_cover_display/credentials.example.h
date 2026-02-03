// credentials.example.h
// Copy this file to credentials.h and fill in your actual values
// DO NOT commit credentials.h to version control!

#pragma once

// Wi-Fi credentials
constexpr const char *WIFI_SSID = "YOUR_WIFI_SSID";
constexpr const char *WIFI_PASS = "YOUR_WIFI_PASSWORD";

// MQTT broker details
constexpr const char *MQTT_HOST = "YOUR_MQTT_BROKER_IP";
constexpr uint16_t MQTT_PORT = 1883;
constexpr const char *MQTT_USER = "YOUR_MQTT_USER";
constexpr const char *MQTT_PASS = "YOUR_MQTT_PASSWORD";

// Optional: Override MQTT topic base (default: "iotstack/shairport-extension")
// #define MQTT_TOPIC_BASE "your/custom/topic"
