<?php
// index.php - Main RIST Monitor Dashboard
require_once 'config/config.php';
require_once 'services/rist-service.php';

$ristService = new RistService();
$transports = $ristService->getTransports();
$activeTransport = isset($_GET['transport']) ? $_GET['transport'] : ($transports[0]['id'] ?? 'default');
?>
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>RIST Monitor</title>
    <link rel="stylesheet" href="assets/css/main.css">
    <link rel="stylesheet" href="assets/css/components.css">
    <!-- Leaflet CSS for maps -->
    <link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"
          integrity="sha256-p4NxAoJBhIIN+hmNHrzRCf9tD/miZyoHS5obTRR9BMY="
          crossorigin=""/>
</head>
<body>
    <?php include 'components/header.php'; ?>
    
    <!-- Transport Tabs -->
    <div class="transponder-tabs">
        <div class="tabs-container">
            <?php foreach ($transports as $transport): ?>
                <div class="tab-wrapper">
                    <button class="tab <?= $transport['id'] === $activeTransport ? 'active' : '' ?>"
                            onclick="switchTransport('<?= $transport['id'] ?>')">
                        <?= htmlspecialchars($transport['name']) ?>
                        <span class="tab-status status-<?= $transport['status'] ?>"></span>
                    </button>
                    <div class="tab-actions">
                        <button class="tab-action-btn"
                                onclick="event.stopPropagation(); transportConfig.editTransport('<?= $transport['id'] ?>')"
                                title="Edit Transport">
                            ✏️
                        </button>
                        <button class="tab-action-btn delete-btn"
                                onclick="event.stopPropagation(); transportConfig.deleteTransport('<?= $transport['id'] ?>', '<?= htmlspecialchars($transport['name']) ?>')"
                                title="Delete Transport">
                            🗑️
                        </button>
                    </div>
                </div>
            <?php endforeach; ?>
            <button class="add-transponder" onclick="showAddTransportModal()">
                + Add Transport
            </button>
        </div>
    </div>

    <div class="main-container">
        <div class="content-area">
            <!-- Transport Status Card - HIDDEN per user request -->
            <div id="transport-status" class="satellite-overview" style="display: none;">
                <!-- Dynamically loaded via JavaScript -->
            </div>

            <!-- World Map -->
            <div class="card">
                <h2 class="card-title">Global Receiver Distribution</h2>
                <div class="map-legend">
                    <span class="legend-item">
                        <span class="legend-dot" style="background: #10b981;"></span>
                        Satellite
                    </span>
                    <span class="legend-item">
                        <span class="legend-dot" style="background: #f59e0b;"></span>
                        FSR
                    </span>
                    <span class="legend-item">
                        <span class="legend-dot" style="background: #ef4444;"></span>
                        Offline
                    </span>
                </div>
                <div id="receiver-map" class="map-container"></div>
            </div>

            <!-- Receivers List -->
            <div class="card receivers-section">
                <h2 class="card-title">Connected Receivers</h2>
                
                <div class="receivers-controls">
                    <input type="text" id="receiver-search" class="search-box" placeholder="Search by Box ID, Location, or IP...">
                    <div class="filter-buttons">
                        <button class="filter-btn active" data-filter="all">All</button>
                        <button class="filter-btn" data-filter="online">Online (<span id="online-count">0</span>)</button>
                        <button class="filter-btn" data-filter="fsr">FSR (<span id="fsr-count">0</span>)</button>
                        <button class="filter-btn" data-filter="offline">Offline (<span id="offline-count">0</span>)</button>
                    </div>
                </div>

                <div class="receivers-list" id="receivers-list">
                    <div class="receivers-header">
                        <div>Box ID</div>
                        <div>Location</div>
                        <div>Status</div>
                        <div>RTT</div>
                        <div>Quality</div>
                    </div>
                    <div id="receivers-content">
                        <!-- Dynamically loaded via JavaScript -->
                    </div>
                </div>

                <!-- Receiver Detail Card -->
                <div id="receiver-detail-card" class="card" style="margin-top: 1.5rem; display: none;">
                    <h2 class="card-title">Selected Receiver Details</h2>
                    <div id="receiver-detail-content">
                        <div style="color: #9ca3af; text-align: center; padding: 2rem;">
                            Click a receiver to view details
                        </div>
                    </div>
                </div>
            </div>
        </div>

        <?php include 'components/sidebar.php'; ?>
    </div>

    <!-- Add Transport Modal -->
    <div id="add-transport-modal" class="modal">
        <div class="modal-content">
            <div class="modal-header">
                <h3>Add New Transport</h3>
                <span class="close" onclick="closeAddTransportModal()">&times;</span>
            </div>
            <form id="add-transport-form" onsubmit="handleTransportForm(event)">
                <div class="form-group">
                    <label for="transport-name">Transport Name</label>
                    <input type="text" id="transport-name" name="name" required>
                </div>
                
                <div class="form-group">
                    <label for="satellite-select">Satellite</label>
                    <select id="satellite-select" name="satellite" required>
                        <!-- Populated via JavaScript -->
                    </select>
                </div>
                
                <div class="form-group">
                    <label for="input-url">Input URL</label>
                    <input type="text" id="input-url" name="input_url" placeholder="udp://@239.5.5.5:5000" required>
                </div>
                
                <div class="form-group">
                    <label>Output URLs</label>
                    <div id="output-urls">
                        <div class="output-url-group">
                            <input type="text" name="output_urls[]" placeholder="rist://@192.168.110.107:5554?weight=0&buffer=8000" required>
                            <button type="button" class="btn-remove" onclick="removeOutputUrl(this)">×</button>
                        </div>
                    </div>
                    <button type="button" onclick="addOutputUrl()">+ Add Output URL</button>
                </div>
                
                <div class="form-actions">
                    <button type="button" onclick="closeAddTransportModal()">Cancel</button>
                    <button type="submit">Create Transport</button>
                </div>
            </form>
        </div>
    </div>

    <!-- Bandwidth Graph Modal -->
    <div id="bandwidth-graph-modal" class="modal">
        <div class="modal-content" style="max-width: 1200px;">
            <div class="modal-header">
                <h3>Bandwidth Graph - <span id="graph-receiver-name"></span></h3>
                <span class="close" onclick="window.bandwidthGraph.hide()">&times;</span>
            </div>

            <div class="graph-controls">
                <div class="metric-selector">
                    <button class="metric-btn active" data-metric="bandwidth" onclick="window.bandwidthGraph.changeMetric('bandwidth')">Bandwidth</button>
                    <button class="metric-btn" data-metric="quality" onclick="window.bandwidthGraph.changeMetric('quality')">Quality</button>
                    <button class="metric-btn" data-metric="rtt" onclick="window.bandwidthGraph.changeMetric('rtt')">RTT</button>
                    <button class="metric-btn" data-metric="packet_loss" onclick="window.bandwidthGraph.changeMetric('packet_loss')">Packet Loss</button>
                    <button class="metric-btn" data-metric="retry_bandwidth" onclick="window.bandwidthGraph.changeMetric('retry_bandwidth')">Retry BW</button>
                </div>

                <div class="range-selector">
                    <button class="range-btn" data-range="5m" onclick="window.bandwidthGraph.changeTimeRange('5m')">5m</button>
                    <button class="range-btn" data-range="15m" onclick="window.bandwidthGraph.changeTimeRange('15m')">15m</button>
                    <button class="range-btn" data-range="30m" onclick="window.bandwidthGraph.changeTimeRange('30m')">30m</button>
                    <button class="range-btn active" data-range="1h" onclick="window.bandwidthGraph.changeTimeRange('1h')">1h</button>
                    <button class="range-btn" data-range="6h" onclick="window.bandwidthGraph.changeTimeRange('6h')">6h</button>
                    <button class="range-btn" data-range="24h" onclick="window.bandwidthGraph.changeTimeRange('24h')">24h</button>
                </div>
            </div>

            <div class="graph-container">
                <canvas id="bandwidth-chart"></canvas>
            </div>

            <div id="graph-stats" class="graph-stats">
                <!-- Populated dynamically -->
            </div>
        </div>
    </div>

    <!-- Leaflet JS for maps -->
    <script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"
            integrity="sha256-20nQCchB9co0qIjJZRGuk2/Z9VM+kNiyxNV1lvTlZBo="
            crossorigin=""></script>

    <!-- Chart.js for graphs -->
    <script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
    <script src="https://cdn.jsdelivr.net/npm/chartjs-adapter-date-fns@3.0.0/dist/chartjs-adapter-date-fns.bundle.min.js"></script>

    <script src="assets/js/api-client.js"></script>
    <script src="assets/js/transport-config.js"></script>
    <script src="assets/js/dashboard.js"></script>
    <script src="assets/js/real-time.js"></script>
    <script src="assets/js/receiver-map.js"></script>
    <script src="assets/js/bandwidth-graph.js"></script>
    
    <script>
        // Initialize dashboard with active transport
        document.addEventListener('DOMContentLoaded', function() {
            Dashboard.init('<?= $activeTransport ?>');
        });
        
        // Handle transport form submission
        async function handleTransportForm(event) {
            event.preventDefault();

            const formData = new FormData(event.target);
            const data = {
                name: formData.get('name'),
                satellite: formData.get('satellite'),
                input_url: formData.get('input_url'),
                output_urls: formData.getAll('output_urls[]').filter(url => url.trim() !== '')
            };

            // Validate data
            const errors = transportConfig.validateTransportData(data);
            if (errors.length > 0) {
                alert('Validation Errors:\n\n' + errors.join('\n'));
                return;
            }

            try {
                const submitButton = event.target.querySelector('button[type="submit"]');
                const originalText = submitButton.textContent;
                submitButton.disabled = true;

                // Check if we're in edit mode or create mode
                const isEditMode = transportConfig.isEditMode();
                const transportId = transportConfig.getEditingTransportId();

                let response;
                if (isEditMode) {
                    // Update existing transport
                    submitButton.textContent = 'Updating...';
                    response = await ApiClient.put(`/transports/${transportId}`, data);
                } else {
                    // Create new transport
                    submitButton.textContent = 'Creating...';
                    response = await ApiClient.post('/transports', data);
                }

                if (response.error) {
                    alert('Error: ' + (response.message || 'Unknown error occurred'));
                    submitButton.disabled = false;
                    submitButton.textContent = originalText;
                } else {
                    alert(isEditMode ? 'Transport updated successfully!' : 'Transport created successfully!');
                    window.closeAddTransportModal();
                    location.reload(); // Refresh to show updated/new transport
                }
            } catch (error) {
                console.error('Form submission error:', error);
                alert('Error: ' + error.message);
                const submitButton = event.target.querySelector('button[type="submit"]');
                submitButton.disabled = false;
            }
        }
    </script>
</body>
</html>