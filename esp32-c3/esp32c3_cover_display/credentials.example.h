// credentials.example.h
// Copy this file to credentials.h and fill in your actual values

#pragma once

// WiFi
constexpr const char *WIFI_SSID = "YOUR_WIFI_SSID";
constexpr const char *WIFI_PASS = "YOUR_WIFI_PASSWORD";

// MQTT broker
constexpr const char *MQTT_HOST = "YOUR_MQTT_BROKER_IP";
constexpr uint16_t MQTT_PORT = 1883;
constexpr const char *MQTT_USER = "YOUR_MQTT_USER";
constexpr const char *MQTT_PASS = "YOUR_MQTT_PASSWORD";
constexpr const char *MQTT_TOPIC_BASE = "topic/base";