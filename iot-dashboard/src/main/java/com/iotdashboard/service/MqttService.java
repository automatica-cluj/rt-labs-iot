package com.iotdashboard.service;

import jakarta.annotation.PostConstruct;
import jakarta.annotation.PreDestroy;
import org.eclipse.paho.client.mqttv3.*;
import org.eclipse.paho.client.mqttv3.persist.MemoryPersistence;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Service;

import java.util.UUID;

@Service
public class MqttService {

    private static final Logger log = LoggerFactory.getLogger(MqttService.class);
    private static final String TOPIC_FILTER = "devices/#";

    private final DeviceService deviceService;
    private MqttClient mqttClient;

    @Value("${mqtt.broker}")
    private String broker;

    @Value("${mqtt.port}")
    private int port;

    public MqttService(DeviceService deviceService) {
        this.deviceService = deviceService;
    }

    @PostConstruct
    public void connect() {
        try {
            String serverUri = "tcp://" + broker + ":" + port;
            String clientId = "iot-dashboard-" + UUID.randomUUID().toString().substring(0, 8);

            // Memory persistence: Paho would otherwise leave a folder named
            // after the client in the working directory on every run.
            mqttClient = new MqttClient(serverUri, clientId, new MemoryPersistence());

            MqttConnectOptions options = new MqttConnectOptions();
            options.setCleanSession(true);
            options.setAutomaticReconnect(true);
            options.setConnectionTimeout(10);

            // Set callback to handle incoming messages and reconnection
            mqttClient.setCallback(new MqttCallbackExtended() {

                @Override
                public void connectComplete(boolean reconnect, String serverURI) {
                    log.info("Connected to MQTT broker: {} (reconnect: {})", serverURI, reconnect);
                    try {
                        mqttClient.subscribe(TOPIC_FILTER);
                        log.info("Subscribed to: {}", TOPIC_FILTER);
                    } catch (MqttException e) {
                        log.error("Failed to subscribe to {}", TOPIC_FILTER, e);
                    }
                }

                @Override
                public void connectionLost(Throwable cause) {
                    log.warn("MQTT connection lost: {}", cause.getMessage());
                }

                @Override
                public void messageArrived(String topic, MqttMessage message) {
                    String payload = new String(message.getPayload());
                    handleMessage(topic, payload);
                }

                @Override
                public void deliveryComplete(IMqttDeliveryToken token) {
                    // Not used for subscriptions
                }
            });

            // Paho reconnects by itself only after a first successful connect,
            // so keep trying until that one succeeds. It runs in its own
            // thread, so the dashboard starts even while the broker is down.
            Thread firstConnect = new Thread(() -> {
                while (true) {
                    try {
                        mqttClient.connect(options);
                        return;
                    } catch (MqttException e) {
                        log.warn("Cannot reach MQTT broker {} ({}), retrying in 5 s", serverUri, e.getMessage());
                    }
                    try {
                        Thread.sleep(5000);
                    } catch (InterruptedException e) {
                        return;
                    }
                }
            }, "mqtt-first-connect");
            firstConnect.setDaemon(true);
            firstConnect.start();

        } catch (MqttException e) {
            log.error("Failed to connect to MQTT broker", e);
        }
    }

    /**
     * Parse topic "devices/{macAddress}/{field}" and update the device.
     */
    private void handleMessage(String topic, String payload) {
        // Expected format: devices/A1B2C3D4E5F6/temperature
        String[] parts = topic.split("/");
        if (parts.length != 3) {
            return;
        }

        String macAddress = parts[1];
        String field = parts[2];

        try {
            deviceService.updateDeviceField(macAddress, field, payload);
        } catch (Exception e) {
            log.error("Error processing message on topic {}: {}", topic, e.getMessage());
        }
    }

    /**
     * Publish a message to an MQTT topic (used for LED commands).
     */
    public void publish(String topic, String payload) {
        try {
            if (mqttClient != null && mqttClient.isConnected()) {
                mqttClient.publish(topic, new MqttMessage(payload.getBytes()));
                log.info("Published to {}: {}", topic, payload);
            } else {
                log.warn("Cannot publish — MQTT client is not connected");
            }
        } catch (MqttException e) {
            log.error("Failed to publish to {}", topic, e);
        }
    }

    @PreDestroy
    public void disconnect() {
        try {
            if (mqttClient != null && mqttClient.isConnected()) {
                mqttClient.disconnect();
                log.info("Disconnected from MQTT broker");
            }
        } catch (MqttException e) {
            log.error("Error disconnecting from MQTT broker", e);
        }
    }
}
