(function() {
    'use strict';

    /* ── API Client ── */
    const API = '/api/v1';

    async function apiGet(path) {
        const res = await fetch(API + path);
        return res.json();
    }

    async function apiGetAuth(path) {
        const res = await fetch(API + path, {
            headers: { 'Authorization': 'Basic ' + btoa(getCredentials()) }
        });
        if (res.status === 401) {
            clearCredentials();
            promptCredentials();
            throw new Error('Unauthorized');
        }
        return res.json();
    }

    async function apiPost(path, body) {
        const res = await fetch(API + path, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Authorization': 'Basic ' + btoa(getCredentials())
            },
            body: JSON.stringify(body)
        });
        if (res.status === 401) {
            clearCredentials();
            promptCredentials();
            throw new Error('Unauthorized');
        }
        return res.json();
    }

    /* ── Credentials ── */
    let _creds = sessionStorage.getItem('web_creds') || '';

    function getCredentials() {
        if (!_creds) promptCredentials();
        return _creds;
    }

    function promptCredentials() {
        var user = prompt('Username:');
        if (!user) return;
        var pass = prompt('Password:');
        if (!pass) return;
        _creds = user + ':' + pass;
        sessionStorage.setItem('web_creds', _creds);
    }

    function clearCredentials() {
        _creds = '';
        sessionStorage.removeItem('web_creds');
    }

    /* ── Toast ── */
    var toastEl = document.getElementById('toast');
    var toastTimer = null;

    function showToast(msg, type) {
        if (toastTimer) clearTimeout(toastTimer);
        toastEl.textContent = msg;
        toastEl.className = 'toast ' + (type || 'info');
        toastTimer = setTimeout(function() {
            toastEl.className = 'toast hidden';
        }, 3000);
    }

    /* ── Navigation ── */
    var navLinks = document.querySelectorAll('.nav-link');
    var views = document.querySelectorAll('.view');

    function switchView(name) {
        views.forEach(function(v) { v.classList.toggle('active', v.id === 'view-' + name); });
        navLinks.forEach(function(l) { l.classList.toggle('active', l.dataset.view === name); });
        if (name === 'dashboard') startPolling();
        else stopPolling();
        if (name === 'settings') loadAllConfigs();
    }

    navLinks.forEach(function(link) {
        link.addEventListener('click', function(e) {
            e.preventDefault();
            var view = this.dataset.view;
            window.location.hash = view;
            switchView(view);
        });
    });

    /* ── Utilities ── */
    function formatUptime(s) {
        var d = Math.floor(s / 86400);
        var h = Math.floor((s % 86400) / 3600);
        var m = Math.floor((s % 3600) / 60);
        if (d > 0) return d + 'd ' + h + 'h ' + m + 'm';
        if (h > 0) return h + 'h ' + m + 'm';
        return m + 'm ' + Math.floor(s % 60) + 's';
    }

    function formatBytes(b) {
        if (b > 1048576) return (b / 1048576).toFixed(1) + ' MB';
        return (b / 1024).toFixed(1) + ' KB';
    }

    /* ── Dashboard: System Info ── */
    var pollTimer = null;

    function updateSystemInfo() {
        apiGet('/status/system').then(function(d) {
            document.getElementById('sys-uptime').textContent = formatUptime(d.uptime_s);
            document.getElementById('sys-heap').textContent = formatBytes(d.heap_free);
            document.getElementById('sys-wifi').textContent = d.wifi_ssid || 'N/A';
            document.getElementById('sys-rssi').textContent = d.wifi_rssi ? d.wifi_rssi + ' dBm' : 'N/A';
            document.getElementById('sys-mqtt').textContent = d.mqtt_connected ? 'Connected' : 'Disconnected';
            document.getElementById('sys-mqtt').style.color = d.mqtt_connected ? 'var(--success)' : 'var(--danger)';
            document.getElementById('sys-ip').textContent = d.ip;
            document.getElementById('sys-time').textContent = d.time;
            document.getElementById('sys-mac').textContent = d.mac;
        }).catch(function() {});
    }

    function updateNodes() {
        apiGet('/status/nodes').then(function(d) {
            var el = document.getElementById('nodes-list');
            if (!d.nodes || d.nodes.length === 0) {
                el.innerHTML = '<p class="empty">No nodes registered</p>';
                return;
            }
            el.innerHTML = d.nodes.map(function(n) {
                var statusClass = n.online ? 'online' : 'offline';
                var statusText = n.online ? 'Online' : 'Offline';
                var sensorHtml = '';
                if (n.sensor) {
                    sensorHtml = '<div class="node-sensor">' + formatSensor(n.sensor) + '</div>';
                }
                return '<div class="node-card">' +
                    '<div class="node-header">' +
                    '<span class="node-status ' + statusClass + '"></span>' +
                    '<span class="node-id">Node ' + n.node_id + '</span>' +
                    ' <span style="font-size:12px;color:var(--text-sec)">' + statusText + '</span>' +
                    '</div>' +
                    '<div class="node-mac">' + n.mac + '</div>' +
                    '<div class="node-meta">' +
                    '<span>Type: ' + n.device_type + '</span>' +
                    '<span>FW: ' + n.fw_version + '</span>' +
                    '<span>RSSI: ' + n.rssi + '</span>' +
                    '</div>' +
                    sensorHtml +
                    '</div>';
            }).join('');
        }).catch(function() {});
    }

    function formatSensor(s) {
        var age = s.age_s < 60 ? s.age_s + 's ago' :
                  s.age_s < 3600 ? Math.floor(s.age_s / 60) + 'm ago' :
                  Math.floor(s.age_s / 3600) + 'h ago';
        var html = '<span class="sensor-age">' + age + '</span>';
        if (s.type === 'env') {
            html += '<div class="sensor-grid">' +
                '<div class="sensor-item"><span class="sensor-label">Temp</span><span class="sensor-val">' + s.temperature.toFixed(1) + '°C</span></div>' +
                '<div class="sensor-item"><span class="sensor-label">Humidity</span><span class="sensor-val">' + s.humidity.toFixed(1) + '%</span></div>' +
                '<div class="sensor-item"><span class="sensor-label">Pressure</span><span class="sensor-val">' + s.pressure.toFixed(0) + ' hPa</span></div>' +
                '<div class="sensor-item"><span class="sensor-label">Light</span><span class="sensor-val">' + s.lux.toFixed(0) + ' lx</span></div>' +
                '</div>';
        } else if (s.type === 'iaq') {
            var aqiLabels = ['','Excellent','Good','Moderate','Poor','Unhealthy'];
            var aqiText = aqiLabels[s.aqi] || ('AQI ' + s.aqi);
            html += '<div class="sensor-grid">' +
                '<div class="sensor-item"><span class="sensor-label">Temp</span><span class="sensor-val">' + s.temperature.toFixed(1) + '°C</span></div>' +
                '<div class="sensor-item"><span class="sensor-label">Humidity</span><span class="sensor-val">' + s.humidity.toFixed(1) + '%</span></div>' +
                '<div class="sensor-item"><span class="sensor-label">eCO₂</span><span class="sensor-val">' + s.eco2 + ' ppm</span></div>' +
                '<div class="sensor-item"><span class="sensor-label">TVOC</span><span class="sensor-val">' + s.tvoc + ' ppb</span></div>' +
                '<div class="sensor-item"><span class="sensor-label">Air Quality</span><span class="sensor-val">' + aqiText + '</span></div>' +
                '</div>';
        } else if (s.type === 'presence') {
            var states = ['None','Moving','Static','Moving+Static'];
            var stateText = states[s.target_state] || ('State ' + s.target_state);
            html += '<div class="sensor-grid">' +
                '<div class="sensor-item"><span class="sensor-label">Target</span><span class="sensor-val">' + stateText + '</span></div>' +
                '<div class="sensor-item"><span class="sensor-label">Move Dist</span><span class="sensor-val">' + s.moving_distance + ' cm</span></div>' +
                '<div class="sensor-item"><span class="sensor-label">Move Energy</span><span class="sensor-val">' + s.moving_energy + '</span></div>' +
                '<div class="sensor-item"><span class="sensor-label">Static Dist</span><span class="sensor-val">' + s.static_distance + ' cm</span></div>' +
                '<div class="sensor-item"><span class="sensor-label">Static Energy</span><span class="sensor-val">' + s.static_energy + '</span></div>' +
                '</div>';
        }
        return html;
    }

    function updateWeather() {
        apiGet('/status/weather').then(function(d) {
            var el = document.getElementById('weather-section');
            if (!d.valid) {
                el.innerHTML = '<p class="empty">Weather data unavailable</p>';
                return;
            }
            var html = '<div class="weather-now">' +
                '<div class="temp">' + d.now.temp + '°C</div>' +
                '<div class="details">' +
                '<div>' + d.now.text + '</div>' +
                '<div>Feels like ' + d.now.feels_like + '°C</div>' +
                '<div>Humidity ' + d.now.humidity + '% | Wind ' + d.now.wind_dir + ' L' + d.now.wind_scale + '</div>' +
                '</div></div>';
            if (d.forecast && d.forecast.length > 0) {
                html += '<div class="forecast">';
                d.forecast.forEach(function(f) {
                    html += '<div class="forecast-card">' +
                        '<div class="date">' + f.date + '</div>' +
                        '<div class="temps">' + f.temp_min + '° / ' + f.temp_max + '°</div>' +
                        '<div class="desc">' + f.text_day + '</div>' +
                        '</div>';
                });
                html += '</div>';
            }
            el.innerHTML = html;
        }).catch(function() {});
    }

    function pollDashboard() {
        updateSystemInfo();
        updateNodes();
        updateWeather();
    }

    function startPolling() {
        pollDashboard();
        if (pollTimer) clearInterval(pollTimer);
        pollTimer = setInterval(pollDashboard, 5000);
    }

    function stopPolling() {
        if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
    }

    /* ── Settings: Load Config ── */
    function loadAllConfigs() {
        apiGetAuth('/config').then(function(d) {
            Object.keys(d).forEach(function(mod) {
                var form = document.querySelector('form[data-module="' + mod + '"]');
                if (!form) return;
                var cfg = d[mod];
                Object.keys(cfg).forEach(function(key) {
                    var input = form.querySelector('input[name="' + key + '"]');
                    if (input) {
                        input.value = cfg[key] === '****' ? '' : cfg[key];
                        if (cfg[key] === '****') input.placeholder = '(unchanged)';
                    }
                });
            });
        }).catch(function(e) {
            if (e.message !== 'Unauthorized') showToast('Failed to load config', 'error');
        });
    }

    /* ── Settings: Save Config ── */
    document.querySelectorAll('form[data-module]').forEach(function(form) {
        form.addEventListener('submit', function(e) {
            e.preventDefault();
            var mod = this.dataset.module;
            var data = {};
            var inputs = this.querySelectorAll('input');
            inputs.forEach(function(input) {
                var val = input.value.trim();
                if (val === '') return; // skip empty (unchanged)
                if (input.type === 'number') data[input.name] = parseInt(val, 10);
                else data[input.name] = val;
            });
            if (Object.keys(data).length === 0) {
                showToast('No changes to save', 'info');
                return;
            }
            apiPost('/config/' + mod, data).then(function(r) {
                if (r.error) { showToast(r.error, 'error'); return; }
                showToast('Saved! ' + (r.restart_required ? 'Restart required.' : ''), 'success');
            }).catch(function(e) {
                if (e.message !== 'Unauthorized') showToast('Save failed', 'error');
            });
        });
    });

    /* ── Settings: Reset Defaults ── */
    document.querySelectorAll('.btn-reset').forEach(function(btn) {
        btn.addEventListener('click', function() {
            var mod = this.dataset.module;
            if (!confirm('Reset ' + mod + ' config to defaults?')) return;
            apiPost('/config/' + mod + '/reset', {}).then(function(r) {
                if (r.error) { showToast(r.error, 'error'); return; }
                showToast(mod + ' reset to defaults. Restart required.', 'success');
                loadAllConfigs();
            }).catch(function(e) {
                if (e.message !== 'Unauthorized') showToast('Reset failed', 'error');
            });
        });
    });

    /* ── Restart ── */
    document.getElementById('btn-restart').addEventListener('click', function() {
        if (!confirm('Restart device?')) return;
        apiPost('/system/restart', {}).then(function() {
            showToast('Device is restarting...', 'info');
        }).catch(function() {
            showToast('Restart request sent', 'info');
        });
    });

    /* ── Init ── */
    var hash = window.location.hash.replace('#', '') || 'dashboard';
    switchView(hash);
    window.addEventListener('hashchange', function() {
        switchView(window.location.hash.replace('#', '') || 'dashboard');
    });
})();
