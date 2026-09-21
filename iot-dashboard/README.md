# IoT Dashboard — Spring Boot Application

A Spring Boot web application that connects to an MQTT broker, receives sensor data from ESP32 devices, stores it in a database, and provides a real-time web dashboard.

## Application Structure

```
src/main/java/com/iotdashboard/
├── IoTDashboardApplication.java        # Entry point
├── model/
│   ├── Device.java                     # JPA entity — device identity + latest readings
│   └── SensorData.java                 # JPA entity — time-series sensor snapshots
├── repository/
│   ├── DeviceRepository.java           # findByMacAddress()
│   └── SensorDataRepository.java       # findTop100ByDeviceOrderByTimestampDesc()
├── dto/
│   ├── DeviceResponse.java             # API response record
│   ├── SensorDataResponse.java         # Sensor history response record
│   └── LedCommandRequest.java          # LED command request with validation
├── service/
│   ├── MqttService.java                # MQTT client — subscribes to broker, publishes LED commands
│   └── DeviceService.java              # Business logic — CRUD, field updates, sensor snapshots
└── controller/
    ├── DeviceController.java           # REST API endpoints
    └── GlobalExceptionHandler.java     # Centralized error handling
```

## Key Components

### MqttService

Connects to the MQTT broker using the Eclipse Paho client library. On startup (`@PostConstruct`), it subscribes to `devices/#` to receive all device messages. When a message arrives on a topic like `devices/AABBCCDDEEFF/temperature`, it parses the topic into MAC address + field name and delegates to `DeviceService.updateDeviceField()`.

Also exposes a `publish()` method used by the controller to send LED commands back to devices.

### DeviceService

Handles all database operations. The `updateDeviceField()` method is the core dispatch — it receives a MAC address, field name, and raw string value, then updates the matching field on the Device entity. When the field is `temperature`, it also saves a `SensorData` snapshot for the time-series history.

Devices are created automatically the first time a message is received from a new MAC address.

### Device Entity

Stores both the device identity (MAC address, IP) and the latest sensor values (temperature, RSSI, heap, uptime, etc.) directly on the entity. This avoids joins when the dashboard fetches all devices with their current readings.

### SensorData Entity

Stores periodic sensor snapshots (one per ESP32 publish cycle). Linked to a Device via `@ManyToOne`. Used by the `/history` endpoint to show trends over time.

## REST API

| Method | Endpoint                    | Description                         |
|--------|-----------------------------|-------------------------------------|
| GET    | `/api/devices`              | List all devices with latest values |
| GET    | `/api/devices/{id}`         | Single device details               |
| GET    | `/api/devices/{id}/history` | Last 100 sensor snapshots           |
| POST   | `/api/devices/{id}/led`     | Send LED command `{"state":"on"}`   |

## Configuration

### Default Profile — H2 (local development)

`application.yml` — in-memory database, no setup needed:

```yaml
spring.datasource.url: jdbc:h2:mem:iotdb
spring.jpa.hibernate.ddl-auto: create-drop
mqtt.broker: control.aut.utcluj.ro
mqtt.port: 11188
```

### Docker Profile — PostgreSQL

`application-docker.yml` — activated via `SPRING_PROFILES_ACTIVE=docker`:

```yaml
spring.datasource.url: jdbc:postgresql://postgres:5432/iotdb
spring.jpa.hibernate.ddl-auto: update
```

## How to Run

### Local (requires Java 17 + Maven)

```bash
mvn spring-boot:run
```

- Dashboard: http://localhost:8080
- H2 Console: http://localhost:8080/h2-console (JDBC URL: `jdbc:h2:mem:iotdb`, user: `sa`, no password)

### Docker (requires Docker)

```bash
docker compose up --build
```

- Dashboard: http://localhost:8080
- Stop: `docker compose down` (add `-v` to delete database volume)

### With a local broker (when the faculty broker cannot be reached)

The dashboard and the devices normally meet on `control.aut.utcluj.ro:11188`.
`compose.local-broker.yml` adds a Mosquitto broker on this computer and points
the dashboard at it:

```bash
docker compose -f compose.yml -f compose.local-broker.yml up --build
```

- The broker listens on port 1883, with no accounts (`mosquitto/mosquitto.conf`).
- The board has to use the same broker. In its `secrets.h` set `MQTT_HOST` to
  the IP address this computer has on the Wi-Fi network, not `localhost`, and
  `MQTT_PORT` to `1883`. Board and computer must be on the same network, and
  the computer's firewall must let port 1883 in.
- A fake device, without a board: `BROKER=localhost PORT=1883 ../simulate-device.sh`
- Stop: `docker compose -f compose.yml -f compose.local-broker.yml down`

Without Docker, any Mosquitto on port 1883 will do. The dashboard takes its
broker from two settings that the environment can override:

```bash
MQTT_BROKER=localhost MQTT_PORT=1883 mvn spring-boot:run
```

## Dependencies

| Dependency                    | Purpose                         |
|-------------------------------|---------------------------------|
| `spring-boot-starter-web`    | REST API + static file serving  |
| `spring-boot-starter-data-jpa` | JPA / Hibernate ORM           |
| `spring-boot-starter-validation` | Request validation (`@Valid`) |
| `h2`                         | In-memory database (dev)        |
| `postgresql`                 | PostgreSQL driver (docker)      |
| `org.eclipse.paho.client.mqttv3` | MQTT client library          |

## Frontend

Located in `src/main/resources/static/`:

| File            | Purpose                                          |
|-----------------|--------------------------------------------------|
| `index.html`    | Single-page dashboard layout                     |
| `css/style.css` | Card grid layout, RSSI color coding, LED buttons |
| `js/app.js`     | Polls `GET /api/devices` every 5s, renders device cards, sends LED commands |

No build tools or frameworks — plain HTML, CSS, and vanilla JavaScript.
