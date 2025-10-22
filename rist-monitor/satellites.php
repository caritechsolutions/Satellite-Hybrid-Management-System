<?php
// satellites.php - Satellite Management Page
require_once 'config/config.php';
require_once 'services/rist-service.php';

// Simple satellite management (reads from config/satellites.json)
$satellitesFile = __DIR__ . '/config/satellites.json';
$satellites = [];

if (file_exists($satellitesFile)) {
    $satellitesData = file_get_contents($satellitesFile);
    $data = json_decode($satellitesData, true) ?: [];
    $satellites = $data['satellites'] ?? [];
}
?>
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Satellite Management - RIST Monitor</title>
    <link rel="stylesheet" href="assets/css/main.css">
    <link rel="stylesheet" href="assets/css/components.css">
</head>
<body>
    <?php include 'components/header.php'; ?>

    <div class="main-container" style="grid-template-columns: 1fr;">
        <div class="content-area">
            <div class="card">
                <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 2rem;">
                    <h1 class="card-title" style="margin: 0;">Satellite Management</h1>
                    <div style="display: flex; gap: 1rem;">
                        <button class="btn btn-secondary" onclick="window.location.href='/'">
                            &#8592; Back to Dashboard
                        </button>
                        <button class="btn btn-success" onclick="showAddSatelliteModal()">
                            + Add Satellite
                        </button>
                    </div>
                </div>

                <!-- Satellites Table -->
                <div style="overflow-x: auto;">
                    <table class="satellites-table">
                        <thead>
                            <tr>
                                <th>Name</th>
                                <th>Position</th>
                                <th>Frequency (MHz)</th>
                                <th>Symbol Rate (KBaud)</th>
                                <th>Bitrate (Mbps)</th>
                                <th>Status</th>
                                <th>Actions</th>
                            </tr>
                        </thead>
                        <tbody id="satellites-tbody">
                            <?php foreach ($satellites as $satellite): ?>
                            <tr>
                                <td><?= htmlspecialchars($satellite['name']) ?></td>
                                <td><?= htmlspecialchars($satellite['position']) ?></td>
                                <td><?= htmlspecialchars($satellite['frequency']) ?></td>
                                <td><?= htmlspecialchars($satellite['symbol_rate']) ?></td>
                                <td><?= htmlspecialchars($satellite['bitrate']) ?></td>
                                <td>
                                    <span class="status-indicator status-<?= $satellite['status'] ?>">
                                        <?= ucfirst($satellite['status']) ?>
                                    </span>
                                </td>
                                <td>
                                    <button class="btn-action btn-edit"
                                            onclick="editSatellite('<?= htmlspecialchars($satellite['id']) ?>')"
                                            title="Edit">
                                        &#9998;
                                    </button>
                                    <button class="btn-action btn-delete"
                                            onclick="deleteSatellite('<?= htmlspecialchars($satellite['id']) ?>', '<?= htmlspecialchars($satellite['name']) ?>')"
                                            title="Delete">
                                        &#128465;
                                    </button>
                                </td>
                            </tr>
                            <?php endforeach; ?>
                            <?php if (empty($satellites)): ?>
                            <tr>
                                <td colspan="7" style="text-align: center; color: #9ca3af; padding: 2rem;">
                                    No satellites configured. Click "Add Satellite" to create one.
                                </td>
                            </tr>
                            <?php endif; ?>
                        </tbody>
                    </table>
                </div>
            </div>
        </div>
    </div>

    <!-- Add/Edit Satellite Modal -->
    <div id="satellite-modal" class="modal">
        <div class="modal-content">
            <div class="modal-header">
                <h3 id="satellite-modal-title">Add New Satellite</h3>
                <span class="close" onclick="closeSatelliteModal()">&times;</span>
            </div>
            <form id="satellite-form" onsubmit="handleSatelliteForm(event)">
                <input type="hidden" id="satellite-id" name="id">

                <div class="form-group">
                    <label for="satellite-name">Satellite Name *</label>
                    <input type="text" id="satellite-name" name="name" required
                           placeholder="e.g., Eutelsat 65W">
                </div>

                <div class="form-group">
                    <label for="satellite-position">Orbital Position *</label>
                    <input type="text" id="satellite-position" name="position" required
                           placeholder="e.g., 65.0W or 19.2E">
                </div>

                <div class="form-group">
                    <label for="satellite-frequency">Frequency (MHz) *</label>
                    <input type="number" id="satellite-frequency" name="frequency" required
                           placeholder="e.g., 11450" min="0" step="1">
                </div>

                <div class="form-group">
                    <label for="satellite-symbol-rate">Symbol Rate (KBaud) *</label>
                    <input type="number" id="satellite-symbol-rate" name="symbol_rate" required
                           placeholder="e.g., 27500" min="0" step="1">
                </div>

                <div class="form-group">
                    <label for="satellite-bitrate">Total Bitrate (Mbps) *</label>
                    <input type="number" id="satellite-bitrate" name="bitrate" required
                           placeholder="e.g., 60.0" min="0" step="0.1">
                </div>

                <div class="form-group">
                    <label for="satellite-status">Status *</label>
                    <select id="satellite-status" name="status" required>
                        <option value="active">Active</option>
                        <option value="inactive">Inactive</option>
                        <option value="maintenance">Maintenance</option>
                    </select>
                </div>

                <div class="form-actions">
                    <button type="button" onclick="closeSatelliteModal()">Cancel</button>
                    <button type="submit" id="satellite-submit-btn">Create Satellite</button>
                </div>
            </form>
        </div>
    </div>

    <script src="assets/js/api-client.js"></script>
    <script>
        let currentEditingSatelliteId = null;

        function showAddSatelliteModal() {
            currentEditingSatelliteId = null;
            document.getElementById('satellite-modal-title').textContent = 'Add New Satellite';
            document.getElementById('satellite-submit-btn').textContent = 'Create Satellite';
            document.getElementById('satellite-form').reset();
            document.getElementById('satellite-id').value = '';
            document.getElementById('satellite-modal').classList.add('show');
        }

        function closeSatelliteModal() {
            document.getElementById('satellite-modal').classList.remove('show');
            currentEditingSatelliteId = null;
        }

        async function editSatellite(satelliteId) {
            try {
                const response = await ApiClient.get(`/satellites.php?id=${satelliteId}`);
                const satellite = response.data;

                currentEditingSatelliteId = satelliteId;

                document.getElementById('satellite-modal-title').textContent = 'Edit Satellite';
                document.getElementById('satellite-submit-btn').textContent = 'Update Satellite';

                document.getElementById('satellite-id').value = satellite.id;
                document.getElementById('satellite-name').value = satellite.name;
                document.getElementById('satellite-position').value = satellite.position;
                document.getElementById('satellite-frequency').value = satellite.frequency;
                document.getElementById('satellite-symbol-rate').value = satellite.symbol_rate;
                document.getElementById('satellite-bitrate').value = satellite.bitrate;
                document.getElementById('satellite-status').value = satellite.status;

                document.getElementById('satellite-modal').classList.add('show');
            } catch (error) {
                console.error('Failed to load satellite:', error);
                alert('Failed to load satellite data: ' + error.message);
            }
        }

        async function deleteSatellite(satelliteId, satelliteName) {
            const confirmed = confirm(
                `Are you sure you want to delete the satellite "${satelliteName}"?\n\n` +
                `This will affect any transports using this satellite.`
            );

            if (!confirmed) return;

            try {
                await ApiClient.delete(`/satellites.php?id=${satelliteId}`);
                alert('Satellite deleted successfully!');
                location.reload();
            } catch (error) {
                console.error('Failed to delete satellite:', error);
                alert('Failed to delete satellite: ' + error.message);
            }
        }

        async function handleSatelliteForm(event) {
            event.preventDefault();

            const formData = new FormData(event.target);
            const data = {
                name: formData.get('name'),
                position: formData.get('position'),
                frequency: formData.get('frequency'),
                symbol_rate: formData.get('symbol_rate'),
                bitrate: formData.get('bitrate'),
                status: formData.get('status')
            };

            try {
                const submitButton = event.target.querySelector('button[type="submit"]');
                const originalText = submitButton.textContent;
                submitButton.disabled = true;

                let response;
                if (currentEditingSatelliteId) {
                    // Update existing satellite
                    submitButton.textContent = 'Updating...';
                    response = await ApiClient.put(`/satellites.php?id=${currentEditingSatelliteId}`, data);
                } else {
                    // Create new satellite
                    submitButton.textContent = 'Creating...';
                    response = await ApiClient.post('/satellites.php', data);
                }

                if (response.error) {
                    alert('Error: ' + (response.message || 'Unknown error occurred'));
                    submitButton.disabled = false;
                    submitButton.textContent = originalText;
                } else {
                    alert(currentEditingSatelliteId ? 'Satellite updated successfully!' : 'Satellite created successfully!');
                    closeSatelliteModal();
                    location.reload();
                }
            } catch (error) {
                console.error('Form submission error:', error);
                alert('Error: ' + error.message);
                const submitButton = event.target.querySelector('button[type="submit"]');
                submitButton.disabled = false;
            }
        }
    </script>
    <style>
        .satellites-table {
            width: 100%;
            border-collapse: collapse;
            background: rgba(31, 41, 55, 0.4);
            border-radius: 0.5rem;
            overflow: hidden;
        }

        .satellites-table thead {
            background: rgba(55, 65, 81, 0.6);
        }

        .satellites-table th,
        .satellites-table td {
            padding: 1rem;
            text-align: left;
            border-bottom: 1px solid rgba(75, 85, 99, 0.3);
        }

        .satellites-table th {
            font-weight: 600;
            color: #f3f4f6;
            font-size: 0.875rem;
            text-transform: uppercase;
            letter-spacing: 0.05em;
        }

        .satellites-table td {
            color: #e5e7eb;
        }

        .satellites-table tbody tr:hover {
            background: rgba(55, 65, 81, 0.3);
        }

        .satellites-table tbody tr:last-child td {
            border-bottom: none;
        }

        .btn-action {
            padding: 0.5rem;
            margin: 0 0.25rem;
            background: rgba(55, 65, 81, 0.8);
            border: 1px solid rgba(75, 85, 99, 0.3);
            border-radius: 0.25rem;
            cursor: pointer;
            font-size: 1rem;
            transition: all 0.2s ease;
            color: #e5e7eb;
        }

        .btn-action:hover {
            background: rgba(75, 85, 99, 0.9);
            transform: scale(1.1);
        }

        .btn-action.btn-delete:hover {
            background: rgba(239, 68, 68, 0.8);
            border-color: #ef4444;
        }

        .btn-action.btn-edit:hover {
            background: rgba(59, 130, 246, 0.8);
            border-color: #3b82f6;
        }
    </style>
</body>
</html>
