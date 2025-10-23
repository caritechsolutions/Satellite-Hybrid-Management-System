<?php
// components/sidebar.php - Sidebar Component
?>

<div class="sidebar">
    <!-- Advanced Features Section -->
    <div class="advanced-features">
        <h3 class="sidebar-title">Advanced Features</h3>
        
        <div class="feature-tabs">
            <button class="feature-tab active" onclick="dashboard.switchFeatureTab('ad-insertion')">Ad Insertion</button>
            <button class="feature-tab" onclick="dashboard.switchFeatureTab('statistics')">Statistics</button>
            <button class="feature-tab" onclick="dashboard.switchFeatureTab('config')">Config</button>
        </div>

        <!-- Ad Insertion Tab -->
        <div class="feature-content active" id="ad-insertion">
            <div class="config-section">
                <div class="config-label">Ad Insertion Mode</div>
                <select class="config-select" id="ad-mode">
                    <option value="disabled">Disabled</option>
                    <option value="scte35">SCTE-35 Triggered</option>
                    <option value="time">Time-based</option>
                    <option value="manual">Manual Control</option>
                </select>
            </div>
            
            <div class="config-section">
                <div class="config-label">Ad Server URL</div>
                <input type="text" class="config-input" id="ad-server-url" 
                       placeholder="https://ad-server.example.com/api">
            </div>
            
            <div class="config-section">
                <div class="config-label">Enable Regional Targeting</div>
                <div class="config-toggle">
                    <div class="toggle-switch" onclick="toggleSwitch(this)">
                        <div class="toggle-slider"></div>
                    </div>
                    <span style="font-size: 0.875rem; color: #9ca3af;">Target ads by receiver location</span>
                </div>
            </div>
            
            <button class="btn btn-primary" style="width: 100%;" onclick="applyAdConfiguration()">
                Apply Ad Configuration
            </button>
        </div>

        <!-- Statistics Tab -->
        <div class="feature-content" id="statistics">
            <div class="stats-grid">
                <div class="stat-card">
                    <div class="stat-card-value" id="stat-active">0</div>
                    <div class="stat-card-label">Active Receivers</div>
                </div>
                <div class="stat-card">
                    <div class="stat-card-value" id="stat-fsr">0</div>
                    <div class="stat-card-label">FSR Mode</div>
                </div>
                <div class="stat-card">
                    <div class="stat-card-value" id="stat-offline">0</div>
                    <div class="stat-card-label">Offline</div>
                </div>
                <div class="stat-card">
                    <div class="stat-card-value" id="stat-uptime">0h</div>
                    <div class="stat-card-label">Uptime</div>
                </div>
            </div>
            
            <div class="config-section" style="margin-top: 1.5rem;">
                <div class="config-label">Export Statistics</div>
                <div style="display: flex; gap: 0.5rem;">
                    <button class="btn btn-secondary" style="flex: 1;" onclick="exportStatistics('csv')">
                        CSV Export
                    </button>
                    <button class="btn btn-secondary" style="flex: 1;" onclick="exportStatistics('json')">
                        JSON Export
                    </button>
                </div>
            </div>
            
            <div class="config-section">
                <div class="config-label">Generate Report</div>
                <button class="btn btn-primary" style="width: 100%;" onclick="generateReport()">
                    &#128196; Generate Full Report
                </button>
            </div>
        </div>

        <!-- Configuration Tab -->
        <div class="feature-content" id="config">
            <div class="config-section">
                <div class="config-label">RIST Profile</div>
                <select class="config-select" id="rist-profile">
                    <option value="simple">Simple Profile</option>
                    <option value="main" selected>Main Profile</option>
                    <option value="advanced">Advanced Profile</option>
                </select>
            </div>
            
            <div class="config-section">
                <div class="config-label">Encryption</div>
                <select class="config-select" id="encryption">
                    <option value="none">None</option>
                    <option value="aes128">AES-128</option>
                    <option value="aes256" selected>AES-256</option>
                </select>
            </div>
            
            <div class="config-section">
                <div class="config-label">Buffer Size (ms)</div>
                <input type="number" class="config-input" id="buffer-size" 
                       value="8000" min="1000" max="30000" step="1000">
            </div>
            
            <div class="config-section">
                <div class="config-label">NULL Packet Deletion</div>
                <div class="config-toggle">
                    <div class="toggle-switch active" onclick="toggleSwitch(this)">
                        <div class="toggle-slider"></div>
                    </div>
                    <span style="font-size: 0.875rem; color: #9ca3af;">Optimize bandwidth</span>
                </div>
            </div>
            
            <div class="config-section">
                <div class="config-label">Metrics Export</div>
                <div class="config-toggle">
                    <div class="toggle-switch active" onclick="toggleSwitch(this)">
                        <div class="toggle-slider"></div>
                    </div>
                    <span style="font-size: 0.875rem; color: #9ca3af;">Enable Prometheus metrics</span>
                </div>
            </div>
            
            <div style="display: flex; gap: 0.5rem; margin-top: 1rem;">
                <button class="btn btn-success" style="flex: 1;" onclick="saveConfiguration()">
                    Save Config
                </button>
                <button class="btn btn-danger" style="flex: 1;" onclick="resetConfiguration()">
                    Reset Defaults
                </button>
            </div>
        </div>
    </div>

    <!-- System Health -->
    <div class="sidebar-card">
        <h3 class="sidebar-title">System Health</h3>
        <ul class="stats-list">
            <li class="stats-item">
                <span class="stats-label">CPU Usage</span>
                <span class="stats-value" id="cpu-usage">-</span>
            </li>
            <li class="stats-item">
                <span class="stats-label">Memory Usage</span>
                <span class="stats-value" id="memory-usage">-</span>
            </li>
            <li class="stats-item">
                <span class="stats-label">Disk Usage</span>
                <span class="stats-value" id="disk-usage">-</span>
            </li>
            <li class="stats-item">
                <span class="stats-label">Network Load</span>
                <span class="stats-value" id="network-load">-</span>
            </li>
        </ul>
    </div>

    <!-- Distribution Health -->
    <div class="sidebar-card">
        <h3 class="sidebar-title">Distribution Health</h3>
        <ul class="stats-list">
            <li class="stats-item">
                <span class="stats-label">Satellite Signal</span>
                <span class="stats-value" style="color: #10b981;" id="satellite-signal">Excellent</span>
            </li>
            <li class="stats-item">
                <span class="stats-label">Recovery Ready</span>
                <span class="stats-value" style="color: #10b981;" id="recovery-ready">Yes</span>
            </li>
            <li class="stats-item">
                <span class="stats-label">Buffer Level</span>
                <span class="stats-value" id="buffer-level">2.1s</span>
            </li>
            <li class="stats-item">
                <span class="stats-label">FSR Events Today</span>
                <span class="stats-value" id="fsr-events">0</span>
            </li>
        </ul>
    </div>

    <!-- Recent Events -->
    <div class="sidebar-card">
        <h3 class="sidebar-title">Recent Events</h3>
        <div id="recent-events" style="font-size: 0.875rem; color: #9ca3af; line-height: 1.4;">
            <div class="loading">Loading events...</div>
        </div>
    </div>
</div>

<script>
// Advanced features functionality
function applyAdConfiguration() {
    const mode = document.getElementById('ad-mode').value;
    const serverUrl = document.getElementById('ad-server-url').value;
    
    // TODO: Send configuration to backend
    console.log('Applying ad configuration:', { mode, serverUrl });
    
    if (window.dashboard) {
        window.dashboard.showSuccess('Ad configuration applied successfully');
    }
}

function exportStatistics(format) {
    // TODO: Generate and download statistics file
    console.log('Exporting statistics in format:', format);
    
    if (window.dashboard) {
        window.dashboard.showSuccess(`Statistics exported as ${format.toUpperCase()}`);
    }
}

function generateReport() {
    // TODO: Generate comprehensive report
    console.log('Generating comprehensive report');
    
    if (window.dashboard) {
        window.dashboard.showSuccess('Report generation started');
    }
}

function saveConfiguration() {
    const config = {
        rist_profile: document.getElementById('rist-profile').value,
        encryption: document.getElementById('encryption').value,
        buffer_size: parseInt(document.getElementById('buffer-size').value),
        null_packet_deletion: document.querySelector('#config .toggle-switch').classList.contains('active'),
        metrics_export: document.querySelector('#config .toggle-switch:last-of-type').classList.contains('active')
    };
    
    // TODO: Send configuration to backend
    console.log('Saving configuration:', config);
    
    if (window.dashboard) {
        window.dashboard.showSuccess('Configuration saved successfully');
    }
}

function resetConfiguration() {
    if (confirm('Are you sure you want to reset all configuration to defaults?')) {
        // Reset form values
        document.getElementById('rist-profile').value = 'main';
        document.getElementById('encryption').value = 'aes256';
        document.getElementById('buffer-size').value = '8000';
        
        // Reset toggles
        document.querySelectorAll('#config .toggle-switch').forEach(toggle => {
            toggle.classList.add('active');
        });
        
        // TODO: Send reset command to backend
        console.log('Resetting configuration to defaults');
        
        if (window.dashboard) {
            window.dashboard.showSuccess('Configuration reset to defaults');
        }
    }
}

// Auto-update system health
function updateSystemHealth() {
    ApiClient.get('/system-health.php')
        .then(response => {
            const data = response.data;

            // Update CPU Usage with color coding
            const cpuElement = document.getElementById('cpu-usage');
            if (cpuElement && data.cpu_usage !== undefined) {
                cpuElement.textContent = data.cpu_usage + '%';

                // Color code based on usage
                if (data.cpu_usage > 80) {
                    cpuElement.style.color = '#ef4444'; // Red
                } else if (data.cpu_usage > 60) {
                    cpuElement.style.color = '#f59e0b'; // Orange
                } else {
                    cpuElement.style.color = '#10b981'; // Green
                }
            }

            // Update Memory Usage with color coding
            const memoryElement = document.getElementById('memory-usage');
            if (memoryElement && data.memory_usage !== undefined) {
                memoryElement.textContent = data.memory_usage + '%';

                // Color code based on usage
                if (data.memory_usage > 80) {
                    memoryElement.style.color = '#ef4444'; // Red
                } else if (data.memory_usage > 60) {
                    memoryElement.style.color = '#f59e0b'; // Orange
                } else {
                    memoryElement.style.color = '#10b981'; // Green
                }
            }

            // Update Disk Usage
            const diskElement = document.getElementById('disk-usage');
            if (diskElement && data.disk_usage) {
                if (data.disk_usage.usage_percentage !== undefined) {
                    diskElement.textContent = data.disk_usage.usage_percentage + '%';

                    // Color code based on usage
                    if (data.disk_usage.usage_percentage > 90) {
                        diskElement.style.color = '#ef4444'; // Red
                    } else if (data.disk_usage.usage_percentage > 75) {
                        diskElement.style.color = '#f59e0b'; // Orange
                    } else {
                        diskElement.style.color = '#10b981'; // Green
                    }
                }
            }

            // Network Load - set to normal for now
            const networkElement = document.getElementById('network-load');
            if (networkElement) {
                networkElement.textContent = 'Normal';
                networkElement.style.color = '#10b981';
            }
        })
        .catch(error => {
            // Silently fail - API not implemented yet
            console.debug('System health API not available');
        });
}

// Auto-update recent events
function updateRecentEvents() {
    ApiClient.get('/system/events?limit=5')
        .then(response => {
            const events = response.data || [];
            const container = document.getElementById('recent-events');

            if (events.length === 0) {
                container.innerHTML = '<div>No recent events</div>';
                return;
            }

            const eventsHTML = events.map(event =>
                `<div style="margin-bottom: 0.5rem;">&#8226; ${event.message} (${formatTimeAgo(event.timestamp)})</div>`
            ).join('');

            container.innerHTML = eventsHTML;
        })
        .catch(error => {
            // Silently fail - API not implemented yet
            console.debug('System events API not available');
            document.getElementById('recent-events').innerHTML = '<div>No events available</div>';
        });
}

function formatTimeAgo(timestamp) {
    const now = new Date();
    const time = new Date(timestamp);
    const diffInSeconds = Math.floor((now - time) / 1000);
    
    if (diffInSeconds < 60) return `${diffInSeconds}s ago`;
    if (diffInSeconds < 3600) return `${Math.floor(diffInSeconds / 60)}m ago`;
    if (diffInSeconds < 86400) return `${Math.floor(diffInSeconds / 3600)}h ago`;
    return `${Math.floor(diffInSeconds / 86400)}d ago`;
}

// Initialize sidebar updates
document.addEventListener('DOMContentLoaded', function() {
    // Initial load
    setTimeout(() => {
        updateSystemHealth();
        updateRecentEvents();
    }, 1000);

    // Update system health every 5 seconds for real-time monitoring
    setInterval(() => {
        updateSystemHealth();
    }, 5000);

    // Update events less frequently (every 30 seconds)
    setInterval(() => {
        updateRecentEvents();
    }, 30000);
});
</script>