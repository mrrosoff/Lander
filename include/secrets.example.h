// Copy this file to include/secrets.h and fill in real values.
// secrets.h is gitignored; secrets.example.h is committed as a template.

#pragma once

// List your WiFi networks (the strongest one in range is used). Add as many as
// you like by adding more WIFI_AP(ssid, pass) lines.
#define WIFI_NETWORKS \
  WIFI_AP("your-ssid",   "your-password") \
  WIFI_AP("second-ssid", "second-password")
