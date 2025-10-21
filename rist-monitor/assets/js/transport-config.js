// assets/js/transport-config.js - Transport Configuration Management

/**
 * Transport Configuration Manager
 * Handles transport form validation, editing, and deletion
 */
class TransportConfig {
    constructor() {
        this.currentEditingTransport = null;
    }

    /**
     * Validate transport form data
     */
    validateTransportData(data) {
        const errors = [];

        // Validate name
        if (!data.name || data.name.trim().length === 0) {
            errors.push('Transport name is required');
        }

        // Validate satellite
        if (!data.satellite) {
            errors.push('Satellite selection is required');
        }

        // Validate input URL
        if (!data.input_url || !this.isValidUrl(data.input_url)) {
            errors.push('Valid input URL is required (e.g., udp://@239.5.5.5:5000)');
        }

        // Validate output URLs
        if (!data.output_urls || data.output_urls.length === 0) {
            errors.push('At least one output URL is required');
        } else {
            data.output_urls.forEach((url, index) => {
                if (!this.isValidUrl(url)) {
                    errors.push(`Output URL ${index + 1} is invalid`);
                }
            });
        }

        return errors;
    }

    /**
     * Basic URL validation for RIST/UDP protocols
     */
    isValidUrl(url) {
        if (!url || url.trim().length === 0) return false;

        // Check for valid protocols
        const validProtocols = ['udp://', 'rist://', 'rtp://'];
        const hasValidProtocol = validProtocols.some(protocol =>
            url.toLowerCase().startsWith(protocol)
        );

        return hasValidProtocol;
    }

    /**
     * Open edit modal and populate with transport data
     */
    async editTransport(transportId) {
        try {
            // Fetch transport data
            const response = await ApiClient.get(`/transports/${transportId}`);
            const transport = response.data;

            this.currentEditingTransport = transportId;

            // Populate form fields
            document.getElementById('transport-name').value = transport.name;
            document.getElementById('satellite-select').value = transport.satellite;
            document.getElementById('input-url').value = transport.input_url;

            // Populate output URLs
            const container = document.getElementById('output-urls');
            container.innerHTML = '';

            transport.output_urls.forEach((url, index) => {
                const group = document.createElement('div');
                group.className = 'output-url-group';
                group.innerHTML = `
                    <input type="text" name="output_urls[]" value="${url}" required>
                    <button type="button" class="btn-remove" onclick="removeOutputUrl(this)">×</button>
                `;
                container.appendChild(group);
            });

            // Update modal title and button
            document.querySelector('#add-transport-modal .modal-header h3').textContent = 'Edit Transport';
            document.querySelector('#add-transport-form button[type="submit"]').textContent = 'Update Transport';

            // Show modal
            document.getElementById('add-transport-modal').classList.add('show');
            await window.loadSatellites();

        } catch (error) {
            console.error('Failed to load transport for editing:', error);
            alert('Failed to load transport data: ' + error.message);
        }
    }

    /**
     * Delete transport with confirmation
     */
    async deleteTransport(transportId, transportName) {
        const confirmed = confirm(
            `Are you sure you want to delete the transport "${transportName}"?\n\n` +
            `This will stop the transport if it's running and permanently remove it from the system.`
        );

        if (!confirmed) return;

        try {
            await ApiClient.delete(`/transports/${transportId}`);

            alert('Transport deleted successfully!');

            // Reload page to show updated transport list
            window.location.href = '/';

        } catch (error) {
            console.error('Failed to delete transport:', error);
            alert('Failed to delete transport: ' + error.message);
        }
    }

    /**
     * Reset the form to "Add" mode
     */
    resetToAddMode() {
        this.currentEditingTransport = null;

        // Reset modal title and button
        document.querySelector('#add-transport-modal .modal-header h3').textContent = 'Add New Transport';
        document.querySelector('#add-transport-form button[type="submit"]').textContent = 'Create Transport';

        // Reset form
        document.getElementById('add-transport-form').reset();

        // Reset output URLs to single field
        const container = document.getElementById('output-urls');
        container.innerHTML = `
            <div class="output-url-group">
                <input type="text" name="output_urls[]" placeholder="rist://@192.168.110.107:5554?weight=0&buffer=8000" required>
                <button type="button" class="btn-remove" onclick="removeOutputUrl(this)">×</button>
            </div>
        `;
    }

    /**
     * Check if currently in edit mode
     */
    isEditMode() {
        return this.currentEditingTransport !== null;
    }

    /**
     * Get the ID of the transport being edited
     */
    getEditingTransportId() {
        return this.currentEditingTransport;
    }
}

// Create global instance
window.transportConfig = new TransportConfig();
