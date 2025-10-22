// assets/js/receiver-map.js - Interactive Receiver Map

class ReceiverMap {
    constructor(elementId) {
        this.elementId = elementId;
        this.map = null;
        this.markers = [];
        this.markerLayer = null;
    }

    /**
     * Initialize the map
     */
    init() {
        // Initialize Leaflet map centered on world view
        this.map = L.map(this.elementId, {
            center: [20, 0], // Centered on equator
            zoom: 2,
            minZoom: 2,
            maxZoom: 18,
            worldCopyJump: true
        });

        // Add OpenStreetMap tiles (free, no API key required)
        L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
            attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors',
            maxZoom: 19
        }).addTo(this.map);

        // Create a layer group for markers
        this.markerLayer = L.layerGroup().addTo(this.map);

        console.log('Receiver map initialized');
    }

    /**
     * Plot receivers on the map
     */
    plotReceivers(receivers) {
        if (!this.map) {
            console.error('Map not initialized');
            return;
        }

        // Clear existing markers
        this.markerLayer.clearLayers();
        this.markers = [];

        // Filter receivers that have valid coordinates
        const validReceivers = receivers.filter(r =>
            r.latitude && r.longitude &&
            !isNaN(r.latitude) && !isNaN(r.longitude)
        );

        if (validReceivers.length === 0) {
            // If no receivers have coordinates, show some sample locations
            this.showSampleLocations();
            return;
        }

        // Plot each receiver
        validReceivers.forEach(receiver => {
            const marker = this.createMarker(receiver);
            this.markers.push(marker);
            this.markerLayer.addLayer(marker);
        });

        // Fit map to show all markers
        if (this.markers.length > 0) {
            const group = new L.featureGroup(this.markers);
            this.map.fitBounds(group.getBounds().pad(0.1));
        }
    }

    /**
     * Create a marker for a receiver
     */
    createMarker(receiver) {
        // Determine marker color based on status
        const color = this.getStatusColor(receiver.status);

        // Create custom icon
        const icon = L.divIcon({
            className: 'custom-marker',
            html: `<div style="
                width: 12px;
                height: 12px;
                background-color: ${color};
                border: 2px solid white;
                border-radius: 50%;
                box-shadow: 0 0 6px rgba(0,0,0,0.5);
            "></div>`,
            iconSize: [16, 16],
            iconAnchor: [8, 8]
        });

        // Create marker
        const marker = L.marker([receiver.latitude, receiver.longitude], {
            icon: icon,
            title: receiver.box_id
        });

        // Create popup content
        const popupContent = `
            <div style="font-family: 'Inter', sans-serif; min-width: 200px;">
                <div style="font-weight: 600; font-size: 1rem; margin-bottom: 0.5rem; color: #1f2937;">
                    ${receiver.box_id}
                </div>
                <div style="font-size: 0.875rem; color: #6b7280; margin-bottom: 0.5rem;">
                    ${receiver.location}
                </div>
                <div style="display: grid; grid-template-columns: repeat(2, 1fr); gap: 0.5rem; margin-bottom: 0.5rem;">
                    <div>
                        <div style="font-size: 0.75rem; color: #9ca3af;">Status</div>
                        <div style="font-weight: 500; color: ${color};">${this.formatStatus(receiver.status)}</div>
                    </div>
                    <div>
                        <div style="font-size: 0.75rem; color: #9ca3af;">RTT</div>
                        <div style="font-weight: 500;">${receiver.rtt || '-'}</div>
                    </div>
                    <div>
                        <div style="font-size: 0.75rem; color: #9ca3af;">Bandwidth</div>
                        <div style="font-weight: 500;">${receiver.bandwidth || '0'} Mbps</div>
                    </div>
                    <div>
                        <div style="font-size: 0.75rem; color: #9ca3af;">Quality</div>
                        <div style="font-weight: 500;">${receiver.quality || '-'}</div>
                    </div>
                </div>
                <div style="font-size: 0.75rem; color: #9ca3af;">
                    IP: ${receiver.ip_address || 'Unknown'}
                </div>
            </div>
        `;

        marker.bindPopup(popupContent);

        return marker;
    }

    /**
     * Get color based on receiver status
     */
    getStatusColor(status) {
        switch (status) {
            case 'online':
                return '#10b981'; // Green
            case 'fsr':
                return '#f59e0b'; // Orange/Yellow
            case 'offline':
                return '#ef4444'; // Red
            default:
                return '#6b7280'; // Gray
        }
    }

    /**
     * Format status text
     */
    formatStatus(status) {
        switch (status) {
            case 'online': return 'Satellite';
            case 'fsr': return 'FSR';
            case 'offline': return 'Offline';
            default: return status;
        }
    }

    /**
     * Show sample locations when no real data is available
     */
    showSampleLocations() {
        const sampleLocations = [
            { lat: 40.7128, lng: -74.0060, name: 'New York, USA', status: 'online' },
            { lat: 51.5074, lng: -0.1278, name: 'London, UK', status: 'online' },
            { lat: 35.6762, lng: 139.6503, name: 'Tokyo, Japan', status: 'fsr' },
            { lat: -33.8688, lng: 151.2093, name: 'Sydney, Australia', status: 'online' },
            { lat: 19.4326, lng: -99.1332, name: 'Mexico City, Mexico', status: 'offline' },
            { lat: -23.5505, lng: -46.6333, name: 'São Paulo, Brazil', status: 'online' },
            { lat: 48.8566, lng: 2.3522, name: 'Paris, France', status: 'online' },
            { lat: 55.7558, lng: 37.6173, name: 'Moscow, Russia', status: 'fsr' }
        ];

        sampleLocations.forEach(loc => {
            const color = this.getStatusColor(loc.status);

            const icon = L.divIcon({
                className: 'custom-marker',
                html: `<div style="
                    width: 12px;
                    height: 12px;
                    background-color: ${color};
                    border: 2px solid white;
                    border-radius: 50%;
                    box-shadow: 0 0 6px rgba(0,0,0,0.5);
                "></div>`,
                iconSize: [16, 16],
                iconAnchor: [8, 8]
            });

            const marker = L.marker([loc.lat, loc.lng], {
                icon: icon,
                title: loc.name
            });

            const popupContent = `
                <div style="font-family: 'Inter', sans-serif;">
                    <div style="font-weight: 600; margin-bottom: 0.5rem;">${loc.name}</div>
                    <div style="font-size: 0.875rem; color: ${color};">${this.formatStatus(loc.status)}</div>
                    <div style="font-size: 0.75rem; color: #9ca3af; margin-top: 0.5rem;">Sample Location</div>
                </div>
            `;

            marker.bindPopup(popupContent);
            this.markerLayer.addLayer(marker);
        });
    }

    /**
     * Update receivers on the map
     */
    update(receivers) {
        this.plotReceivers(receivers);
    }

    /**
     * Resize map (useful when container size changes)
     */
    resize() {
        if (this.map) {
            this.map.invalidateSize();
        }
    }
}

// Create global instance
window.receiverMap = null;

// Initialize map when document is ready
document.addEventListener('DOMContentLoaded', function() {
    setTimeout(() => {
        if (document.getElementById('receiver-map')) {
            window.receiverMap = new ReceiverMap('receiver-map');
            window.receiverMap.init();

            // If dashboard has receivers, plot them
            if (window.dashboard && window.dashboard.receivers) {
                window.receiverMap.plotReceivers(window.dashboard.receivers);
            }
        }
    }, 500);
});
