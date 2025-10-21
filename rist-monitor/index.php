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
</head>
<body>
    <?php include 'components/header.php'; ?>
    
    <!-- Transport Tabs -->
    <div class="transponder-tabs">
        <div class="tabs-container">
            <?php foreach ($transports as $transport): ?>
                <button class="tab <?= $transport['id'] === $activeTransport ? 'active' : '' ?>" 
                        onclick="switchTransport('<?= $transport['id'] ?>')">
                    <?= htmlspecialchars($transport['name']) ?>
                    <span class="tab-status status-<?= $transport['status'] ?>"></span>
                </button>
            <?php endforeach; ?>
            <button class="add-transponder" onclick="showAddTransportModal()">
                + Add Transport
            </button>
        </div>
    </div>

    <div class="main-container">
        <div class="content-area">
            <!-- Transport Status Card -->
            <div id="transport-status" class="satellite-overview">
                <!-- Dynamically loaded via JavaScript -->
            </div>

            <!-- World Map -->
            <div class="card">
                <h2 class="card-title">Global Receiver Distribution</h2>
                <div class="map-container">
                    <div class="map-placeholder">
                        ??? Interactive World Map<br>
                        <small style="opacity: 0.7;">Green: Satellite • Yellow: FSR • Red: Offline</small>
                    </div>
                </div>
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

    <script src="assets/js/api-client.js"></script>
    <script src="assets/js/transport-config.js"></script>
    <script src="assets/js/dashboard.js"></script>
    <script src="assets/js/real-time.js"></script>
    
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
            
            try {
                const submitButton = event.target.querySelector('button[type="submit"]');
                const originalText = submitButton.textContent;
                submitButton.disabled = true;
                submitButton.textContent = 'Creating...';
                
                const response = await fetch('/api/transports.php', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify(data)
                });
                
                const result = await response.json();
                
                if (result.error) {
                    alert('Error: ' + result.message);
                } else {
                    alert('Transport created successfully!');
                    window.closeAddTransportModal();
                    location.reload(); // Refresh to show new transport
                }
            } catch (error) {
                alert('Error creating transport: ' + error.message);
            }
        }
    </script>
</body>
</html>