package com.iotdashboard.controller;

import com.iotdashboard.dto.DeviceResponse;
import com.iotdashboard.dto.LedCommandRequest;
import com.iotdashboard.dto.SensorDataResponse;
import com.iotdashboard.service.DeviceService;
import com.iotdashboard.service.MqttService;
import jakarta.validation.Valid;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.*;

import java.util.List;

@RestController
@RequestMapping("/api/devices")
public class DeviceController {

    private final DeviceService deviceService;
    private final MqttService mqttService;

    public DeviceController(DeviceService deviceService, MqttService mqttService) {
        this.deviceService = deviceService;
        this.mqttService = mqttService;
    }

    @GetMapping
    public ResponseEntity<List<DeviceResponse>> getAllDevices() {
        return ResponseEntity.ok(deviceService.findAll());
    }

    @GetMapping("/{id}")
    public ResponseEntity<DeviceResponse> getDevice(@PathVariable Long id) {
        return ResponseEntity.ok(deviceService.findById(id));
    }

    @GetMapping("/{id}/history")
    public ResponseEntity<List<SensorDataResponse>> getHistory(@PathVariable Long id) {
        return ResponseEntity.ok(deviceService.getHistory(id));
    }

    @PostMapping("/{id}/led")
    public ResponseEntity<Void> sendLedCommand(@PathVariable Long id,
                                                @Valid @RequestBody LedCommandRequest request) {
        DeviceResponse device = deviceService.findById(id);
        String topic = "devices/" + device.macAddress() + "/led";
        mqttService.publish(topic, request.state());
        return ResponseEntity.ok().build();
    }
}
