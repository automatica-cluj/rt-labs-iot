package com.iotdashboard.model;

import jakarta.persistence.*;
import java.time.LocalDateTime;

@Entity
@Table(name = "sensor_data")
public class SensorData {

    @Id
    @GeneratedValue(strategy = GenerationType.IDENTITY)
    private Long id;

    @ManyToOne
    @JoinColumn(name = "device_id", nullable = false)
    private Device device;

    @Column(nullable = false)
    private LocalDateTime timestamp;

    private Double temperature;
    private Integer rssi;
    private Long freeHeap;
    private Long uptime;
    private Integer wifiChannel;

    public SensorData() {
    }

    public SensorData(Device device, LocalDateTime timestamp, Double temperature,
                      Integer rssi, Long freeHeap, Long uptime, Integer wifiChannel) {
        this.device = device;
        this.timestamp = timestamp;
        this.temperature = temperature;
        this.rssi = rssi;
        this.freeHeap = freeHeap;
        this.uptime = uptime;
        this.wifiChannel = wifiChannel;
    }

    // --- Getters and Setters ---

    public Long getId() {
        return id;
    }

    public void setId(Long id) {
        this.id = id;
    }

    public Device getDevice() {
        return device;
    }

    public void setDevice(Device device) {
        this.device = device;
    }

    public LocalDateTime getTimestamp() {
        return timestamp;
    }

    public void setTimestamp(LocalDateTime timestamp) {
        this.timestamp = timestamp;
    }

    public Double getTemperature() {
        return temperature;
    }

    public void setTemperature(Double temperature) {
        this.temperature = temperature;
    }

    public Integer getRssi() {
        return rssi;
    }

    public void setRssi(Integer rssi) {
        this.rssi = rssi;
    }

    public Long getFreeHeap() {
        return freeHeap;
    }

    public void setFreeHeap(Long freeHeap) {
        this.freeHeap = freeHeap;
    }

    public Long getUptime() {
        return uptime;
    }

    public void setUptime(Long uptime) {
        this.uptime = uptime;
    }

    public Integer getWifiChannel() {
        return wifiChannel;
    }

    public void setWifiChannel(Integer wifiChannel) {
        this.wifiChannel = wifiChannel;
    }
}
