// assets/js/real-time.js - Real-time Updates and WebSocket Management

/**
 * Real-time Connection Manager
 * Handles WebSocket connections and fallback to polling
 */
class RealTimeManager {
    constructor() {
        this.ws = null;
        this.pollingInterval = null;
        this.pollingRate = 60000; // 60 seconds (reduced from 5 to prevent rate limiting)
        this.reconnectAttempts = 0;
        this.maxReconnectAttempts = 3; // Reduced from 5 attempts
        this.connectionMode = 'disabled'; // Disabled by default, can be 'websocket' or 'polling'
    }

    /**
     * Initialize real-time updates
     */
    start() {
        // Disabled by default to prevent excessive polling
        // Can be enabled later when WebSocket server is implemented
        console.log('Real-time manager: disabled (to enable, call realTimeManager.enable())');
        this.connectionMode = 'disabled';
    }

    /**
     * Enable real-time updates
     */
    enable() {
        // Try WebSocket first, fallback to polling
        if (this.supportsWebSocket()) {
            this.startWebSocket();
        } else {
            this.startPolling();
        }
    }

    /**
     * Stop all real-time updates
     */
    stop() {
        this.stopWebSocket();
        this.stopPolling();
    }

    /**
     * Check if WebSocket is supported
     */
    supportsWebSocket() {
        return 'WebSocket' in window;
    }

    /**
     * Start WebSocket connection
     */
    startWebSocket() {
        try {
            const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
            const wsUrl = `${protocol}//${window.location.host}/ws`;

            this.ws = new WebSocket(wsUrl);

            this.ws.onopen = () => {
                console.log('WebSocket connected');
                this.connectionMode = 'websocket';
                this.reconnectAttempts = 0;

                // Subscribe to transport updates
                this.ws.send(JSON.stringify({
                    type: 'subscribe',
                    channel: 'transports'
                }));
            };

            this.ws.onmessage = (event) => {
                try {
                    const data = JSON.parse(event.data);
                    this.handleRealtimeUpdate(data);
                } catch (error) {
                    console.error('Failed to parse WebSocket message:', error);
                }
            };

            this.ws.onerror = (error) => {
                console.error('WebSocket error:', error);
            };

            this.ws.onclose = () => {
                console.log('WebSocket disconnected');
                this.connectionMode = 'polling';

                // Try to reconnect
                if (this.reconnectAttempts < this.maxReconnectAttempts) {
                    this.reconnectAttempts++;
                    setTimeout(() => {
                        console.log(`Reconnecting... (attempt ${this.reconnectAttempts})`);
                        this.startWebSocket();
                    }, 1000 * this.reconnectAttempts);
                } else {
                    // Fallback to polling
                    console.log('Max reconnect attempts reached, falling back to polling');
                    this.startPolling();
                }
            };

        } catch (error) {
            console.error('Failed to start WebSocket:', error);
            this.startPolling();
        }
    }

    /**
     * Stop WebSocket connection
     */
    stopWebSocket() {
        if (this.ws) {
            this.ws.close();
            this.ws = null;
        }
    }

    /**
     * Start polling for updates
     */
    startPolling() {
        if (this.pollingInterval) {
            return; // Already polling
        }

        console.log('Starting polling mode');
        this.connectionMode = 'polling';

        // Poll for updates
        this.pollingInterval = setInterval(() => {
            this.pollForUpdates();
        }, this.pollingRate);

        // Initial poll
        this.pollForUpdates();
    }

    /**
     * Stop polling
     */
    stopPolling() {
        if (this.pollingInterval) {
            clearInterval(this.pollingInterval);
            this.pollingInterval = null;
        }
    }

    /**
     * Poll for updates via HTTP
     */
    async pollForUpdates() {
        try {
            if (!window.dashboard || !window.dashboard.currentTransport) {
                return;
            }

            const transportId = window.dashboard.currentTransport;

            // Get transport status
            const response = await ApiClient.get(`/transports/${transportId}/status`);

            if (response.data) {
                this.handleRealtimeUpdate({
                    type: 'transport_update',
                    data: response.data
                });
            }

        } catch (error) {
            // Silently fail polling errors to avoid spam
            console.debug('Polling error:', error);
        }
    }

    /**
     * Handle real-time update
     */
    handleRealtimeUpdate(update) {
        switch (update.type) {
            case 'transport_update':
                if (window.dashboard) {
                    window.dashboard.updateTransportStatus(update.data);
                }
                break;

            case 'receiver_update':
                if (window.dashboard) {
                    window.dashboard.updateReceiverData(update.data);
                }
                break;

            case 'system_alert':
                this.showSystemAlert(update.data);
                break;

            default:
                console.log('Unknown update type:', update.type);
        }
    }

    /**
     * Show system alert
     */
    showSystemAlert(alert) {
        if (window.dashboard) {
            window.dashboard.showNotification(alert.message, alert.level || 'info');
        }
    }

    /**
     * Get current connection status
     */
    getStatus() {
        return {
            mode: this.connectionMode,
            connected: this.connectionMode === 'websocket' ?
                (this.ws && this.ws.readyState === WebSocket.OPEN) :
                (this.pollingInterval !== null)
        };
    }
}

// Create global instance (but don't start automatically)
window.realTimeManager = new RealTimeManager();

// Auto-start when dashboard is ready
document.addEventListener('DOMContentLoaded', () => {
    // Wait for dashboard to initialize
    setTimeout(() => {
        if (window.dashboard) {
            window.realTimeManager.start();
        }
    }, 1000);
});

// Cleanup on page unload
window.addEventListener('beforeunload', () => {
    window.realTimeManager.stop();
});
