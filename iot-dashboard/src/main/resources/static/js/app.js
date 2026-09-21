// ─── IoT Dashboard ─────────────────────────────────────────────
// Polls the REST API every 5 seconds and renders device cards.

const API_BASE = '/api';

// ─── API Calls ─────────────────────────────────────────────────

async function fetchDevices() {
    const response = await fetch(API_BASE + '/devices');
    if (!response.ok) {
        throw new Error('Failed to fetch devices');
    }
    return response.json();
}

async function sendLedCommand(deviceId, state) {
    const response = await fetch(API_BASE + '/devices/' + deviceId + '/led', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ state: state })
    });
    if (!response.ok) {
        throw new Error('Failed to send LED command');
    }
}

// ─── Rendering ─────────────────────────────────────────────────

function formatUptime(seconds) {
    if (seconds == null) return '—';
    var h = Math.floor(seconds / 3600);
    var m = Math.floor((seconds % 3600) / 60);
    var s = seconds % 60;
    if (h > 0) return h + 'h ' + m + 'm ' + s + 's';
    if (m > 0) return m + 'm ' + s + 's';
    return s + 's';
}

function formatHeap(bytes) {
    if (bytes == null) return '—';
    return Math.round(bytes / 1024) + ' KB';
}

function rssiClass(rssi) {
    if (rssi == null) return '';
    if (rssi >= -50) return 'rssi-good';
    if (rssi >= -70) return 'rssi-medium';
    return 'rssi-weak';
}

function formatTime(isoString) {
    if (!isoString) return 'never';
    var date = new Date(isoString);
    // Show the date too when it is not today, so an old timestamp looks old
    if (date.toDateString() !== new Date().toDateString()) {
        return date.toLocaleDateString() + ' ' + date.toLocaleTimeString();
    }
    return date.toLocaleTimeString();
}

function renderDeviceCard(device) {
    var macDisplay = device.macFormatted || device.macAddress;
    var ledOn = device.ledState === 'on';

    var html = ''
        + '<div class="device-card' + (device.online ? '' : ' offline') + '">'
        + '  <div class="card-header">'
        + '    <div>'
        + '      <div class="mac-address">' + macDisplay + '</div>'
        + '      <div class="ip-address">' + (device.ipAddress || '—') + '</div>'
        + '    </div>'
        + '    <div class="card-status">'
        + '      <span class="status-badge ' + (device.online ? 'online' : 'offline') + '">'
        +          (device.online ? 'ONLINE' : 'OFFLINE') + '</span>'
        + '      <div class="last-seen">Last seen: ' + formatTime(device.lastSeen) + '</div>'
        + '    </div>'
        + '  </div>'
        + '  <div class="readings">'
        + '    <div class="reading">'
        + '      <div class="label">Temperature</div>'
        + '      <div class="value">' + (device.temperature != null ? device.temperature + ' &deg;C' : '&mdash;') + '</div>'
        + '    </div>'
        + '    <div class="reading">'
        + '      <div class="label">WiFi RSSI</div>'
        + '      <div class="value ' + rssiClass(device.rssi) + '">' + (device.rssi != null ? device.rssi + ' dBm' : '&mdash;') + '</div>'
        + '    </div>'
        + '    <div class="reading">'
        + '      <div class="label">Free Heap</div>'
        + '      <div class="value">' + formatHeap(device.freeHeap) + '</div>'
        + '    </div>'
        + '    <div class="reading">'
        + '      <div class="label">Uptime</div>'
        + '      <div class="value">' + formatUptime(device.uptime) + '</div>'
        + '    </div>'
        + '    <div class="reading">'
        + '      <div class="label">WiFi Channel</div>'
        + '      <div class="value">' + (device.wifiChannel != null ? device.wifiChannel : '&mdash;') + '</div>'
        + '    </div>'
        + '  </div>'
        + '  <div class="led-controls">'
        + '    <span class="led-label">'
        + '      <span class="led-status ' + (ledOn ? 'on' : 'off') + '"></span>'
        + '      LED: ' + (device.ledState || 'unknown')
        + '    </span>'
        + '    <button class="led-btn on-btn" onclick="toggleLed(' + device.id + ', \'on\')">ON</button>'
        + '    <button class="led-btn off-btn" onclick="toggleLed(' + device.id + ', \'off\')">OFF</button>'
        + '  </div>'
        + '</div>';

    return html;
}

function renderDashboard(devices) {
    var grid = document.getElementById('devices-grid');
    var countEl = document.getElementById('device-count');
    var refreshEl = document.getElementById('last-refresh');

    countEl.textContent = devices.length + ' device' + (devices.length !== 1 ? 's' : '');
    refreshEl.textContent = 'Updated: ' + new Date().toLocaleTimeString();

    if (devices.length === 0) {
        grid.innerHTML = '<p class="placeholder">Waiting for devices to connect...</p>';
        return;
    }

    var html = '';
    for (var i = 0; i < devices.length; i++) {
        html += renderDeviceCard(devices[i]);
    }
    grid.innerHTML = html;
}

// ─── Actions ───────────────────────────────────────────────────

async function toggleLed(deviceId, state) {
    try {
        await sendLedCommand(deviceId, state);
    } catch (error) {
        alert('Failed to send LED command: ' + error.message);
    }
}

// ─── Polling Loop ──────────────────────────────────────────────

async function loadDevices() {
    try {
        var devices = await fetchDevices();
        renderDashboard(devices);
    } catch (error) {
        console.error('Error loading devices:', error);
    }
}

// Load immediately, then refresh every 5 seconds
loadDevices();
setInterval(loadDevices, 5000);
