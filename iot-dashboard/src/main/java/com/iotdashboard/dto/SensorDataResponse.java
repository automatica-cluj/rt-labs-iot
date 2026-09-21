package com.iotdashboard.dto;

import java.time.LocalDateTime;

public record SensorDataResponse(
        Long id,
        LocalDateTime timestamp,
        Double temperature,
        Integer rssi,
        Long freeHeap,
        Long uptime,
        Integer wifiChannel
) {}
