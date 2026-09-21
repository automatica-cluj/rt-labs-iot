package com.iotdashboard.repository;

import com.iotdashboard.model.Device;
import com.iotdashboard.model.SensorData;
import org.springframework.data.jpa.repository.JpaRepository;

import java.util.List;

public interface SensorDataRepository extends JpaRepository<SensorData, Long> {

    List<SensorData> findTop100ByDeviceOrderByTimestampDesc(Device device);
}
