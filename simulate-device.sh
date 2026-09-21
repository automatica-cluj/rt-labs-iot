#!/bin/sh
# ─────────────────────────────────────────────────────────────────
# simulate-device.sh — Simulates an ESP32 device publishing
# sensor data to the MQTT broker, matching the real sketch behavior.
#
# Usage:
#   ./simulate-device.sh                  # uses defaults
#   ./simulate-device.sh AABBCCDDEEFF     # custom MAC address
#
# Requirements: mosquitto_pub (install with brew install mosquitto
#               or apt install mosquitto-clients)
# ─────────────────────────────────────────────────────────────────

# Override from the environment to use another broker, for example the local one:
#   BROKER=localhost PORT=1883 ./simulate-device.sh
BROKER="${BROKER:-control.aut.utcluj.ro}"
PORT="${PORT:-11188}"
MAC="${1:-AABBCCDDEEFF}"
MAC_FORMATTED="$(echo "$MAC" | sed 's/\(..\)/\1:/g; s/:$//')"
IP="192.168.1.100"
CHANNEL=6
UPTIME=0
LED="off"

# Check that mosquitto_pub is available
if ! command -v mosquitto_pub > /dev/null 2>&1; then
    echo "Error: mosquitto_pub not found."
    echo "Install it with:"
    echo "  macOS:  brew install mosquitto"
    echo "  Linux:  sudo apt install mosquitto-clients"
    exit 1
fi

echo "Simulating ESP32 device"
echo "  MAC:    $MAC_FORMATTED ($MAC)"
echo "  Broker: $BROKER:$PORT"
echo "  Topics: devices/$MAC/*"
echo ""
echo "Press Ctrl+C to stop"
echo "───────────────────────────────────"

while true; do
    # Generate random sensor values
    TEMP=$(awk 'BEGIN { srand(); printf "%.1f", 25 + rand() * 15 }')
    RSSI=$(awk 'BEGIN { srand(); printf "%d", -30 - int(rand() * 50) }')
    HEAP=$(awk 'BEGIN { srand(); printf "%d", 250000 + int(rand() * 50000) }')

    # Publish all fields (same order as the ESP32 sketch)
    mosquitto_pub -h "$BROKER" -p "$PORT" -t "devices/$MAC/mac" -m "$MAC_FORMATTED"
    mosquitto_pub -h "$BROKER" -p "$PORT" -t "devices/$MAC/ip" -m "$IP"
    mosquitto_pub -h "$BROKER" -p "$PORT" -t "devices/$MAC/status" -m "$UPTIME"
    mosquitto_pub -h "$BROKER" -p "$PORT" -t "devices/$MAC/rssi" -m "$RSSI"
    mosquitto_pub -h "$BROKER" -p "$PORT" -t "devices/$MAC/heap" -m "$HEAP"
    mosquitto_pub -h "$BROKER" -p "$PORT" -t "devices/$MAC/wifi_channel" -m "$CHANNEL"
    mosquitto_pub -h "$BROKER" -p "$PORT" -t "devices/$MAC/led_state" -m "$LED"
    mosquitto_pub -h "$BROKER" -p "$PORT" -t "devices/$MAC/temperature" -m "$TEMP"

    UPTIME=$((UPTIME + 5))

    echo "[$MAC] temp=${TEMP}C  rssi=${RSSI}dBm  heap=${HEAP}  uptime=${UPTIME}s"
    sleep 5
done
