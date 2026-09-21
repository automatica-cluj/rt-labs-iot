package com.iotdashboard.dto;

import java.time.LocalDateTime;

public record DeviceResponse(
        Long id,
        String macAddress,
        String macFormatted,
        String ipAddress,
        Double temperature,
        Integer rssi,
        Long freeHeap,
        Long uptime,
        Integer wifiChannel,
        String ledState,
        LocalDateTime lastSeen,
        boolean online
) {}
