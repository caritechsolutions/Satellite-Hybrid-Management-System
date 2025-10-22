// assets/js/bandwidth-graph.js - Bandwidth Graph Component

class BandwidthGraph {
    constructor() {
        this.chart = null;
        this.updateInterval = null;
        this.currentReceiver = null;
        this.currentMetric = 'bandwidth';
        this.currentTimeRange = '1h';
    }

    async show(receiver) {
        this.currentReceiver = receiver;

        // Get peer data
        const peerData = receiver._peer_data;
        if (!peerData) {
            alert('No peer data available for this receiver');
            return;
        }

        // Show modal
        const modal = document.getElementById('bandwidth-graph-modal');
        modal.classList.add('show');

        // Update title
        document.getElementById('graph-receiver-name').textContent =
            `${receiver.box_id} - ${receiver.location}`;

        // Load initial data
        await this.loadData();

        // Start auto-refresh (every 10 seconds)
        this.startAutoRefresh();
    }

    hide() {
        const modal = document.getElementById('bandwidth-graph-modal');
        modal.classList.remove('show');

        // Stop auto-refresh
        if (this.updateInterval) {
            clearInterval(this.updateInterval);
            this.updateInterval = null;
        }

        // Destroy chart
        if (this.chart) {
            this.chart.destroy();
            this.chart = null;
        }

        this.currentReceiver = null;
    }

    async loadData() {
        if (!this.currentReceiver || !this.currentReceiver._peer_data) return;

        const peerData = this.currentReceiver._peer_data;
        const transportId = this.currentReceiver.transport_id;

        try {
            // Show loading
            this.setLoading(true);

            // Fetch historical data
            const response = await ApiClient.get(
                `/metrics-history.php?transport_id=${transportId}&peer_id=${peerData.peer_id}&metric=${this.currentMetric}&range=${this.currentTimeRange}`
            );

            if (response.error) {
                throw new Error(response.message);
            }

            // Update chart
            this.updateChart(response.data);

            // Update stats
            this.updateStats(response.data);

        } catch (error) {
            console.error('Failed to load metrics:', error);
            this.showError(error.message);
        } finally {
            this.setLoading(false);
        }
    }

    updateChart(response) {
        const ctx = document.getElementById('bandwidth-chart').getContext('2d');

        // Prepare data for Chart.js
        const chartData = response.data.map(point => ({
            x: point.timestamp,
            y: point.value
        }));

        // Metric configuration
        const metricConfig = {
            bandwidth: { label: 'Bandwidth (Mbps)', color: '#10b981', unit: 'Mbps' },
            quality: { label: 'Quality (%)', color: '#3b82f6', unit: '%' },
            rtt: { label: 'RTT (ms)', color: '#f59e0b', unit: 'ms' },
            packet_loss: { label: 'Packet Loss (%)', color: '#ef4444', unit: '%' },
            retry_bandwidth: { label: 'Retry Bandwidth (Mbps)', color: '#8b5cf6', unit: 'Mbps' }
        };

        const config = metricConfig[this.currentMetric];

        // Destroy existing chart
        if (this.chart) {
            this.chart.destroy();
        }

        // Create new chart
        this.chart = new Chart(ctx, {
            type: 'line',
            data: {
                datasets: [{
                    label: config.label,
                    data: chartData,
                    borderColor: config.color,
                    backgroundColor: config.color + '20',
                    fill: true,
                    tension: 0.4,
                    pointRadius: 0,
                    pointHoverRadius: 4,
                    borderWidth: 2
                }]
            },
            options: {
                responsive: true,
                maintainAspectRatio: false,
                interaction: {
                    intersect: false,
                    mode: 'index'
                },
                plugins: {
                    legend: {
                        display: true,
                        position: 'top',
                        labels: {
                            color: '#9ca3af',
                            font: { size: 12 }
                        }
                    },
                    tooltip: {
                        backgroundColor: '#1f2937',
                        titleColor: '#f3f4f6',
                        bodyColor: '#f3f4f6',
                        borderColor: '#374151',
                        borderWidth: 1,
                        callbacks: {
                            label: (context) => {
                                return `${config.label}: ${context.parsed.y.toFixed(2)} ${config.unit}`;
                            },
                            title: (items) => {
                                const date = new Date(items[0].parsed.x);
                                return date.toLocaleString();
                            }
                        }
                    }
                },
                scales: {
                    x: {
                        type: 'time',
                        time: {
                            displayFormats: {
                                minute: 'HH:mm',
                                hour: 'HH:mm',
                                day: 'MMM d'
                            }
                        },
                        grid: {
                            color: '#374151',
                            display: true
                        },
                        ticks: {
                            color: '#9ca3af',
                            maxRotation: 0,
                            autoSkipPadding: 20
                        }
                    },
                    y: {
                        beginAtZero: true,
                        grid: {
                            color: '#374151',
                            display: true
                        },
                        ticks: {
                            color: '#9ca3af',
                            callback: (value) => value.toFixed(1) + ' ' + config.unit
                        }
                    }
                }
            }
        });
    }

    updateStats(response) {
        const data = response.data;
        if (data.length === 0) {
            document.getElementById('graph-stats').innerHTML =
                '<div style="color: #9ca3af;">No data available</div>';
            return;
        }

        // Calculate stats
        const values = data.map(p => p.value);
        const current = values[values.length - 1];
        const avg = values.reduce((a, b) => a + b, 0) / values.length;
        const max = Math.max(...values);
        const min = Math.min(...values);

        // Determine unit
        const units = {
            bandwidth: 'Mbps',
            quality: '%',
            rtt: 'ms',
            packet_loss: '%',
            retry_bandwidth: 'Mbps'
        };
        const unit = units[this.currentMetric];

        // Update DOM
        document.getElementById('graph-stats').innerHTML = `
            <div class="stat-item">
                <div class="stat-label">Current</div>
                <div class="stat-value">${current.toFixed(2)} ${unit}</div>
            </div>
            <div class="stat-item">
                <div class="stat-label">Average</div>
                <div class="stat-value">${avg.toFixed(2)} ${unit}</div>
            </div>
            <div class="stat-item">
                <div class="stat-label">Maximum</div>
                <div class="stat-value">${max.toFixed(2)} ${unit}</div>
            </div>
            <div class="stat-item">
                <div class="stat-label">Minimum</div>
                <div class="stat-value">${min.toFixed(2)} ${unit}</div>
            </div>
            <div class="stat-item">
                <div class="stat-label">Data Points</div>
                <div class="stat-value">${response.returned_points}</div>
            </div>
        `;
    }

    changeMetric(metric) {
        this.currentMetric = metric;

        // Update active button
        document.querySelectorAll('.metric-btn').forEach(btn => {
            btn.classList.toggle('active', btn.dataset.metric === metric);
        });

        this.loadData();
    }

    changeTimeRange(range) {
        this.currentTimeRange = range;

        // Update active button
        document.querySelectorAll('.range-btn').forEach(btn => {
            btn.classList.toggle('active', btn.dataset.range === range);
        });

        this.loadData();
    }

    setLoading(loading) {
        const canvas = document.getElementById('bandwidth-chart');
        const parent = canvas.parentElement;

        if (loading) {
            parent.style.opacity = '0.5';
            parent.style.pointerEvents = 'none';
        } else {
            parent.style.opacity = '1';
            parent.style.pointerEvents = 'auto';
        }
    }

    showError(message) {
        const ctx = document.getElementById('bandwidth-chart').getContext('2d');

        if (this.chart) {
            this.chart.destroy();
        }

        // Show error message
        document.getElementById('graph-stats').innerHTML = `
            <div style="color: #ef4444; text-align: center;">
                <strong>Error Loading Data</strong><br>
                ${message}<br><br>
                <small>Make sure the metrics collector is running:<br>
                sudo systemctl status metrics-collector</small>
            </div>
        `;
    }

    startAutoRefresh() {
        // Refresh every 10 seconds
        this.updateInterval = setInterval(() => {
            this.loadData();
        }, 10000);
    }
}

// Create global instance
window.bandwidthGraph = new BandwidthGraph();
