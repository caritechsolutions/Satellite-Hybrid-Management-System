// assets/js/dashboard.js - Main Dashboard Functionality

class Dashboard {
    constructor() {
        this.currentTransport = null;
        this.selectedReceiver = null;
        this.updateInterval = null;
        this.receivers = [];
        this.filteredReceivers = [];
        this.currentFilter = 'all';
        this.searchTerm = '';
        
        this.initializeEventListeners();
    }
    
    static init(transportId) {
        window.dashboard = new Dashboard();
        window.dashboard.loadTransport(transportId);
        window.dashboard.startRealTimeUpdates();
    }
    
    initializeEventListeners() {
        // Search functionality
        const searchBox = document.getElementById('receiver-search');
        if (searchBox) {
            searchBox.addEventListener('input', (e) => {
                this.searchTerm = e.target.value.toLowerCase();
                this.filterReceivers();
            });
        }
        
        // Filter buttons
        document.querySelectorAll('.filter-btn').forEach(btn => {
            btn.addEventListener('click', (e) => {
                const filter = e.target.dataset.filter;
                this.setFilter(filter);
            });
        });
        
        // Advanced features tabs
        document.querySelectorAll('.feature-tab').forEach(tab => {
            tab.addEventListener('click', (e) => {
                this.switchFeatureTab(e.target.textContent.toLowerCase().replace(' ', '-'));
            });
        });
        
        // Transport control buttons
        document.addEventListener('click', (e) => {
            if (e.target.matches('[data-action]')) {
                const action = e.target.dataset.action;
                const transportId = e.target.dataset.transport || this.currentTransport;
                this.handleTransportAction(action, transportId);
            }
        });
    }
    
    async loadTransport(transportId) {
        try {
            showLoading('transport-status');

            const response = await ApiClient.get(`/transports/${transportId}`);
            this.currentTransport = transportId;

            this.updateTransportStatus(response.data);
            this.loadReceivers(transportId);

        } catch (error) {
            console.error('Failed to load transport:', error);
            this.showError('Failed to load transport data');
        }
    }
    
    updateTransportStatus(transport) {
        const statusContainer = document.getElementById('transport-status');
        if (!statusContainer) return;
        
        const satellite = this.getSatelliteInfo(transport.satellite);
        const status = transport.runtime_status || {};
        
        statusContainer.innerHTML = `
            <span class="satellite-icon pulse">&#128752;</span>
            <div class="satellite-name">${satellite.name}</div>
            <div style="color: #9ca3af; margin-bottom: 1rem;">${transport.name}</div>

            <div class="satellite-details">
                <div class="satellite-metric">
                    <div class="satellite-metric-value">${satellite.frequency || '11,450'}</div>
                    <div class="satellite-metric-label">Frequency (MHz)</div>
                </div>
                <div class="satellite-metric">
                    <div class="satellite-metric-value">${satellite.symbol_rate || '27,500'}</div>
                    <div class="satellite-metric-label">Symbol Rate (KBaud)</div>
                </div>
                <div class="satellite-metric">
                    <div class="satellite-metric-value">${satellite.bitrate || '60.0'}</div>
                    <div class="satellite-metric-label">Total Bitrate (Mbps)</div>
                </div>
                <div class="satellite-metric">
                    <div class="satellite-metric-value status-indicator status-${this.getStatusClass(status.status)}">${this.getStatusText(status.status)}</div>
                    <div class="satellite-metric-label">Status</div>
                </div>
            </div>

            <div class="control-buttons">
                ${this.getControlButtons(transport.id, status.status)}
            </div>
        `;
        
        // Update status class
        statusContainer.className = `satellite-overview ${this.getStatusClass(status.status)}`;
    }
    
    getControlButtons(transportId, status) {
        if (status === 'running') {
            return `
                <button class="btn btn-danger" data-action="stop" data-transport="${transportId}">
                    Stop Transport
                </button>
                <button class="btn btn-secondary" data-action="restart" data-transport="${transportId}">
                    Restart
                </button>
            `;
        } else {
            return `
                <button class="btn btn-success" data-action="start" data-transport="${transportId}">
                    Start Transport
                </button>
            `;
        }
    }
    
    getStatusClass(status) {
        switch (status) {
            case 'running': return 'active';
            case 'stopped': return 'inactive';
            case 'error': return 'error';
            default: return 'inactive';
        }
    }
    
    getStatusText(status) {
        switch (status) {
            case 'running': return 'Running';
            case 'stopped': return 'Stopped';
            case 'error': return 'Error';
            case 'starting': return 'Starting';
            default: return 'Unknown';
        }
    }
    
    async loadReceivers(transportId) {
        try {
            showLoading('receivers-content');

            const response = await ApiClient.get(`/transports/${transportId}/receivers`);
            this.receivers = response.data || [];

            this.filterReceivers();
            this.updateReceiverCounts();

            // Update map with receivers
            if (window.receiverMap) {
                window.receiverMap.update(this.receivers);
            }

        } catch (error) {
            console.error('Failed to load receivers:', error);
            document.getElementById('receivers-content').innerHTML =
                '<div class="loading">Failed to load receivers</div>';
        }
    }
    
    filterReceivers() {
        this.filteredReceivers = this.receivers.filter(receiver => {
            // Apply search filter
            if (this.searchTerm) {
                const searchFields = [
                    receiver.box_id,
                    receiver.location,
                    receiver.ip_address
                ].join(' ').toLowerCase();
                
                if (!searchFields.includes(this.searchTerm)) {
                    return false;
                }
            }
            
            // Apply status filter
            if (this.currentFilter !== 'all') {
                if (this.currentFilter !== receiver.status) {
                    return false;
                }
            }
            
            return true;
        });
        
        this.renderReceivers();
    }
    
    renderReceivers() {
        const container = document.getElementById('receivers-content');
        if (!container) return;
        
        if (this.filteredReceivers.length === 0) {
            container.innerHTML = '<div class="loading">No receivers found</div>';
            return;
        }
        
        const receiversHTML = this.filteredReceivers.map(receiver => `
            <div class="receiver-item ${receiver.box_id === this.selectedReceiver?.box_id ? 'selected' : ''}" 
                 onclick="dashboard.selectReceiver('${receiver.box_id}')">
                <div style="font-family: 'JetBrains Mono', monospace; font-weight: 600;">${receiver.box_id}</div>
                <div class="receiver-location">${receiver.location}</div>
                <div class="receiver-status">
                    <span class="status-dot status-${receiver.status}"></span>
                    <span>${this.formatStatus(receiver.status)}</span>
                </div>
                <div class="receiver-metric">${receiver.rtt || '-'}</div>
                <div class="receiver-metric">${receiver.quality || '-'}</div>
            </div>
        `).join('');
        
        container.innerHTML = receiversHTML;
    }
    
    formatStatus(status) {
        switch (status) {
            case 'online': return 'Satellite';
            case 'fsr': return 'FSR';
            case 'offline': return 'Offline';
            default: return status;
        }
    }
    
    updateReceiverCounts() {
        const counts = {
            online: this.receivers.filter(r => r.status === 'online').length,
            fsr: this.receivers.filter(r => r.status === 'fsr').length,
            offline: this.receivers.filter(r => r.status === 'offline').length
        };
        
        // Update count displays
        const onlineCount = document.getElementById('online-count');
        const fsrCount = document.getElementById('fsr-count');
        const offlineCount = document.getElementById('offline-count');
        
        if (onlineCount) onlineCount.textContent = counts.online;
        if (fsrCount) fsrCount.textContent = counts.fsr;
        if (offlineCount) offlineCount.textContent = counts.offline;
        
        // Update header badge
        const totalReceivers = counts.online + counts.fsr + counts.offline;
        const headerBadge = document.querySelector('.status-indicators .status-badge:last-child span:last-child');
        if (headerBadge) {
            headerBadge.textContent = `${totalReceivers.toLocaleString()} Receivers`;
        }
    }
    
    setFilter(filter) {
        this.currentFilter = filter;
        
        // Update active filter button
        document.querySelectorAll('.filter-btn').forEach(btn => {
            btn.classList.toggle('active', btn.dataset.filter === filter);
        });
        
        this.filterReceivers();
    }
    
    selectReceiver(boxId) {
        const receiver = this.receivers.find(r => r.box_id === boxId);
        if (!receiver) return;
        
        this.selectedReceiver = receiver;
        this.renderReceivers(); // Re-render to show selection
        this.updateReceiverDetail(receiver);
    }
    
    updateReceiverDetail(receiver) {
        const detailContainer = document.querySelector('.receiver-detail');
        if (!detailContainer) return;
        
        detailContainer.innerHTML = `
            <div class="detail-header">
                <div class="detail-title">${receiver.box_id} - ${receiver.location}</div>
                <div style="display: flex; align-items: center; gap: 0.5rem;">
                    <span class="status-dot status-${receiver.status}"></span>
                    <span>${this.formatStatus(receiver.status)}</span>
                </div>
            </div>
            
            <div class="detail-metrics">
                <div class="detail-metric">
                    <div class="detail-metric-value">${receiver.bandwidth || '0'} Mbps</div>
                    <div class="detail-metric-label">Bandwidth</div>
                </div>
                <div class="detail-metric">
                    <div class="detail-metric-value">${receiver.rtt || '-'}</div>
                    <div class="detail-metric-label">RTT</div>
                </div>
                <div class="detail-metric">
                    <div class="detail-metric-value">${receiver.packet_loss || '0'}%</div>
                    <div class="detail-metric-label">Packet Loss</div>
                </div>
                <div class="detail-metric">
                    <div class="detail-metric-value">${receiver.quality || '-'}</div>
                    <div class="detail-metric-label">Signal Quality</div>
                </div>
            </div>
            
            <div class="detail-info">
                <div class="detail-info-label">IP Address</div>
                <div class="detail-info-value">${receiver.ip_address || 'Unknown'}</div>
            </div>
            
            <button class="btn btn-primary" style="width: 100%; margin-top: 1rem;" 
                    onclick="dashboard.showBandwidthGraph('${receiver.box_id}')">
                ?? Show Bandwidth Graph
            </button>
        `;
    }
    
    async handleTransportAction(action, transportId) {
        try {
            const btn = event.target;
            const originalText = btn.textContent;
            btn.disabled = true;
            btn.textContent = 'Please wait...';
            
            await ApiClient.post(`/transports/${transportId}/${action}`);
            
            // Reload transport data
            await this.loadTransport(transportId);
            
            this.showSuccess(`Transport ${action} completed successfully`);
            
        } catch (error) {
            console.error(`Failed to ${action} transport:`, error);
            this.showError(`Failed to ${action} transport: ${error.message}`);
        } finally {
            // Re-enable button (will be replaced by new state)
            setTimeout(() => {
                const btn = event.target;
                btn.disabled = false;
                btn.textContent = originalText;
            }, 1000);
        }
    }
    
    switchFeatureTab(tabName) {
        // Remove active class from all tabs and content
        document.querySelectorAll('.feature-tab').forEach(tab => 
            tab.classList.remove('active'));
        document.querySelectorAll('.feature-content').forEach(content => 
            content.classList.remove('active'));
        
        // Add active class to selected tab and content
        document.querySelector(`.feature-tab[onclick*="${tabName}"]`)?.classList.add('active');
        document.getElementById(tabName)?.classList.add('active');
    }
    
    showBandwidthGraph(boxId) {
        // This would open a modal with Chart.js graph
        alert(`Bandwidth graph for ${boxId} would open here`);
        
        // TODO: Implement modal with Chart.js
        // - Fetch historical data from API
        // - Create time-series chart
        // - Show real-time updates
    }
    
    startRealTimeUpdates() {
        // Update every 30 seconds (reduced from 5 to prevent rate limiting)
        // Only update receivers, not the entire transport
        this.updateInterval = setInterval(() => {
            if (this.currentTransport) {
                this.loadReceivers(this.currentTransport);
            }
        }, 30000);
    }
    
    stopRealTimeUpdates() {
        if (this.updateInterval) {
            clearInterval(this.updateInterval);
            this.updateInterval = null;
        }
    }
    
    getSatelliteInfo(satelliteId) {
        // This would typically come from an API call
        const satellites = {
            'eutelsat_65w': {
                name: 'Eutelsat 65W',
                frequency: '11,450',
                symbol_rate: '27,500',
                bitrate: '60.0'
            },
            'ses_17': {
                name: 'SES-17',
                frequency: '12,050',
                symbol_rate: '30,000',
                bitrate: '75.0'
            },
            'astra_19e': {
                name: 'Astra 19.2�E',
                frequency: '11,739',
                symbol_rate: '22,000',
                bitrate: '45.0'
            }
        };
        
        return satellites[satelliteId] || {
            name: 'Unknown Satellite',
            frequency: '0',
            symbol_rate: '0',
            bitrate: '0'
        };
    }
    
    showSuccess(message) {
        this.showNotification(message, 'success');
    }
    
    showError(message) {
        this.showNotification(message, 'error');
    }
    
    showNotification(message, type) {
        // Create notification element
        const notification = document.createElement('div');
        notification.className = `alert alert-${type}`;
        notification.textContent = message;
        notification.style.position = 'fixed';
        notification.style.top = '20px';
        notification.style.right = '20px';
        notification.style.zIndex = '1000';
        notification.style.minWidth = '300px';
        
        document.body.appendChild(notification);
        
        // Remove after 5 seconds
        setTimeout(() => {
            notification.remove();
        }, 5000);
    }
}

// Utility functions
function showLoading(elementId) {
    const element = document.getElementById(elementId);
    if (element) {
        element.innerHTML = '<div class="loading"><div class="loading-spinner"></div>Loading...</div>';
    }
}

function switchTransport(transportId) {
    if (window.dashboard) {
        window.dashboard.loadTransport(transportId);
    }
    
    // Update URL without page reload
    const url = new URL(window.location);
    url.searchParams.set('transport', transportId);
    window.history.pushState({}, '', url);
}

// Global functions for onclick handlers
window.switchTransport = switchTransport;

window.showAddTransportModal = function() {
    // Reset to add mode
    transportConfig.resetToAddMode();

    // Show modal and load satellites
    document.getElementById('add-transport-modal').classList.add('show');
    window.loadSatellites();
};

window.closeAddTransportModal = function() {
    document.getElementById('add-transport-modal').classList.remove('show');

    // Reset to add mode when closing
    transportConfig.resetToAddMode();
};

window.loadSatellites = async function() {
    try {
        const response = await fetch('/api/satellites.php');
        const result = await response.json();
        
        const select = document.getElementById('satellite-select');
        select.innerHTML = '<option value="">Select Satellite...</option>';
        
        result.data.forEach(satellite => {
            const option = document.createElement('option');
            option.value = satellite.id;
            option.textContent = `${satellite.name} (${satellite.position})`;
            select.appendChild(option);
        });
        
    } catch (error) {
        console.error('Error loading satellites:', error);
    }
};

window.addOutputUrl = function() {
    const container = document.getElementById('output-urls');
    const newGroup = document.createElement('div');
    newGroup.className = 'output-url-group';
    newGroup.innerHTML = `
        <input type="text" name="output_urls[]" placeholder="rist://@192.168.1.107:5555?weight=1000&buffer=8000" required>
        <button type="button" class="btn-remove" onclick="removeOutputUrl(this)">×</button>
    `;
    container.appendChild(newGroup);
};

window.removeOutputUrl = function(button) {
    // Only allow removal if there's more than one output URL
    const container = document.getElementById('output-urls');
    const groups = container.querySelectorAll('.output-url-group');

    if (groups.length > 1) {
        button.parentElement.remove();
    } else {
        alert('At least one output URL is required');
    }
};

window.toggleSwitch = function(element) {
    element.classList.toggle('active');
};