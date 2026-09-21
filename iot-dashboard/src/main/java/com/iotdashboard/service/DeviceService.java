package com.iotdashboard.service;

import com.iotdashboard.dto.DeviceResponse;
import com.iotdashboard.dto.SensorDataResponse;
import com.iotdashboard.model.Device;
import com.iotdashboard.model.SensorData;
import com.iotdashboard.repository.DeviceRepository;
import com.iotdashboard.repository.SensorDataRepository;
import org.springframework.data.domain.Sort;
import org.springframework.http.HttpStatus;
import org.springframework.stereotype.Service;
import org.springframework.transaction.annotation.Transactional;
import org.springframework.web.server.ResponseStatusException;

import java.time.LocalDateTime;
import java.util.List;

@Service
public class DeviceService {

    // The firmware publishes telemetry every 5 s. A device that missed three
    // rounds in a row is reported offline; one late message is not enough.
    private static final long ONLINE_TIMEOUT_SECONDS = 15;

    private final DeviceRepository deviceRepository;
    private final SensorDataRepository sensorDataRepository;

    public DeviceService(DeviceRepository deviceRepository,
                         SensorDataRepository sensorDataRepository) {
        this.deviceRepository = deviceRepository;
        this.sensorDataRepository = sensorDataRepository;
    }

    public List<DeviceResponse> findAll() {
        return deviceRepository.findAll(Sort.by("id")).stream()
                .map(this::toResponse)
                .toList();
    }

    public DeviceResponse findById(Long id) {
        Device device = deviceRepository.findById(id)
                .orElseThrow(() -> new ResponseStatusException(HttpStatus.NOT_FOUND, "Device not found"));
        return toResponse(device);
    }

    public List<SensorDataResponse> getHistory(Long deviceId) {
        Device device = deviceRepository.findById(deviceId)
                .orElseThrow(() -> new ResponseStatusException(HttpStatus.NOT_FOUND, "Device not found"));
        return sensorDataRepository.findTop100ByDeviceOrderByTimestampDesc(device).stream()
                .map(this::toSensorResponse)
                .toList();
    }

    /**
     * Called by MqttService when a message arrives on a device topic.
     * Updates the matching field on the Device entity and saves it.
     * When field is "temperature", also creates a sensor data snapshot.
     */
    @Transactional
    public void updateDeviceField(String macAddress, String field, String value) {
        Device device = getOrCreateDevice(macAddress);
        device.setLastSeen(LocalDateTime.now());

        switch (field) {
            case "temperature" -> {
                device.setTemperature(parseDouble(value));
                saveSensorSnapshot(device);
            }
            case "rssi" -> device.setRssi(parseInt(value));
            case "heap" -> device.setFreeHeap(parseLong(value));
            case "status" -> device.setUptime(parseLong(value));
            case "wifi_channel" -> device.setWifiChannel(parseInt(value));
            case "ip" -> device.setIpAddress(value.trim());
            case "mac" -> device.setMacFormatted(value.trim());
            case "led_state" -> device.setLedState(value.trim());
        }

        deviceRepository.save(device);
    }

    private Device getOrCreateDevice(String macAddress) {
        return deviceRepository.findByMacAddress(macAddress)
                .orElseGet(() -> {
                    Device newDevice = new Device(macAddress);
                    return deviceRepository.save(newDevice);
                });
    }

    private void saveSensorSnapshot(Device device) {
        SensorData snapshot = new SensorData(
                device,
                LocalDateTime.now(),
                device.getTemperature(),
                device.getRssi(),
                device.getFreeHeap(),
                device.getUptime(),
                device.getWifiChannel()
        );
        sensorDataRepository.save(snapshot);
    }

    // --- Safe parsing helpers ---

    private Double parseDouble(String value) {
        try {
            return Double.parseDouble(value.trim());
        } catch (NumberFormatException e) {
            return null;
        }
    }

    private Integer parseInt(String value) {
        try {
            return Integer.parseInt(value.trim());
        } catch (NumberFormatException e) {
            return null;
        }
    }

    private Long parseLong(String value) {
        try {
            return Long.parseLong(value.trim());
        } catch (NumberFormatException e) {
            return null;
        }
    }

    // --- DTO mapping ---

    private DeviceResponse toResponse(Device device) {
        return new DeviceResponse(
                device.getId(),
                device.getMacAddress(),
                device.getMacFormatted(),
                device.getIpAddress(),
                device.getTemperature(),
                device.getRssi(),
                device.getFreeHeap(),
                device.getUptime(),
                device.getWifiChannel(),
                device.getLedState(),
                device.getLastSeen(),
                isOnline(device)
        );
    }

    // Decided here, not in the browser: lastSeen has no timezone, and the
    // server compares it against its own clock.
    private boolean isOnline(Device device) {
        LocalDateTime lastSeen = device.getLastSeen();
        return lastSeen != null
                && lastSeen.isAfter(LocalDateTime.now().minusSeconds(ONLINE_TIMEOUT_SECONDS));
    }

    private SensorDataResponse toSensorResponse(SensorData data) {
        return new SensorDataResponse(
                data.getId(),
                data.getTimestamp(),
                data.getTemperature(),
                data.getRssi(),
                data.getFreeHeap(),
                data.getUptime(),
                data.getWifiChannel()
        );
    }
}
