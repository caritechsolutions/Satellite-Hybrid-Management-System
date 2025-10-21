// assets/js/api-client.js - API Client for RIST Monitor

class ApiClient {
    static baseURL = '/api';
    static timeout = 30000; // 30 seconds
    
    static async request(method, endpoint, data = null, options = {}) {
        const url = `${this.baseURL}${endpoint}`;
        
        const config = {
            method: method.toUpperCase(),
            headers: {
                'Content-Type': 'application/json',
                'X-Requested-With': 'XMLHttpRequest',
                ...options.headers
            },
            ...options
        };
        
        // Add CSRF token for state-changing requests
        if (['POST', 'PUT', 'DELETE'].includes(config.method)) {
            const csrfToken = this.getCSRFToken();
            if (csrfToken) {
                if (config.method === 'DELETE') {
                    config.headers['X-CSRF-Token'] = csrfToken;
                } else if (data) {
                    data.csrf_token = csrfToken;
                }
            }
        }
        
        // Add request body for POST/PUT requests
        if (data && ['POST', 'PUT'].includes(config.method)) {
            config.body = JSON.stringify(data);
        }
        
        try {
            // Create abort controller for timeout
            const controller = new AbortController();
            const timeoutId = setTimeout(() => controller.abort(), this.timeout);
            config.signal = controller.signal;
            
            const response = await fetch(url, config);
            clearTimeout(timeoutId);
            
            // Parse response
            const responseData = await this.parseResponse(response);
            
            if (!response.ok) {
                throw new ApiError(
                    responseData.message || `HTTP ${response.status}`,
                    response.status,
                    responseData.error_code
                );
            }
            
            return responseData;
            
        } catch (error) {
            if (error.name === 'AbortError') {
                throw new ApiError('Request timeout', 408);
            }
            
            if (error instanceof ApiError) {
                throw error;
            }
            
            // Network or other errors
            throw new ApiError(
                error.message || 'Network error occurred',
                0
            );
        }
    }
    
    static async parseResponse(response) {
        const contentType = response.headers.get('content-type');
        
        if (contentType && contentType.includes('application/json')) {
            return await response.json();
        }
        
        const text = await response.text();
        if (text) {
            try {
                return JSON.parse(text);
            } catch {
                return { message: text };
            }
        }
        
        return {};
    }
    
    static async get(endpoint, options = {}) {
        return this.request('GET', endpoint, null, options);
    }
    
    static async post(endpoint, data = {}, options = {}) {
        return this.request('POST', endpoint, data, options);
    }
    
    static async put(endpoint, data = {}, options = {}) {
        return this.request('PUT', endpoint, data, options);
    }
    
    static async delete(endpoint, options = {}) {
        return this.request('DELETE', endpoint, null, options);
    }
    
    // Transport-specific methods
    static async getTransports() {
        return this.get('/transports');
    }
    
    static async getTransport(id) {
        return this.get(`/transports/${id}`);
    }
    
    static async createTransport(data) {
        return this.post('/transports', data);
    }
    
    static async updateTransport(id, data) {
        return this.put(`/transports/${id}`, data);
    }
    
    static async deleteTransport(id) {
        return this.delete(`/transports/${id}`);
    }
    
    static async startTransport(id) {
        return this.post(`/transports/${id}/start`);
    }
    
    static async stopTransport(id) {
        return this.post(`/transports/${id}/stop`);
    }
    
    static async restartTransport(id) {
        return this.post(`/transports/${id}/restart`);
    }
    
    static async getTransportStatus(id) {
        return this.get(`/transports/${id}/status`);
    }
    
    static async getTransportMetrics(id) {
        return this.get(`/transports/${id}/metrics`);
    }
    
    static async getTransportReceivers(id) {
        return this.get(`/transports/${id}/receivers`);
    }
    
    static async getTransportLogs(id, lines = 100) {
        return this.get(`/transports/${id}/logs?lines=${lines}`);
    }
    
    // Satellite methods
    static async getSatellites() {
        return this.get('/satellites');
    }
    
    static async getSatellite(id) {
        return this.get(`/satellites/${id}`);
    }
    
    // System methods
    static async getSystemHealth() {
        return this.get('/system/health');
    }
    
    static async getSystemLogs(lines = 100) {
        return this.get(`/system/logs?lines=${lines}`);
    }
    
    // Utility methods
    static getCSRFToken() {
        // Try to get CSRF token from meta tag first
        const metaToken = document.querySelector('meta[name="csrf-token"]');
        if (metaToken) {
            return metaToken.getAttribute('content');
        }
        
        // Try to get from hidden input
        const inputToken = document.querySelector('input[name="csrf_token"]');
        if (inputToken) {
            return inputToken.value;
        }
        
        // Generate and store new token
        return this.generateCSRFToken();
    }
    
    static generateCSRFToken() {
        // Generate a simple CSRF token
        const array = new Uint8Array(32);
        crypto.getRandomValues(array);
        const token = Array.from(array, byte => byte.toString(16).padStart(2, '0')).join('');
        
        // Store in session storage for reuse
        sessionStorage.setItem('csrf_token', token);
        return token;
    }
    
    // Batch operations
    static async batchRequest(requests) {
        const promises = requests.map(req => 
            this.request(req.method, req.endpoint, req.data, req.options)
        );
        
        try {
            const results = await Promise.allSettled(promises);
            return results.map(result => ({
                success: result.status === 'fulfilled',
                data: result.status === 'fulfilled' ? result.value : null,
                error: result.status === 'rejected' ? result.reason : null
            }));
        } catch (error) {
            throw new ApiError('Batch request failed', 0);
        }
    }
    
    // File upload helper
    static async uploadFile(endpoint, file, data = {}, onProgress = null) {
        const formData = new FormData();
        formData.append('file', file);
        
        // Add additional data
        Object.keys(data).forEach(key => {
            formData.append(key, data[key]);
        });
        
        // Add CSRF token
        const csrfToken = this.getCSRFToken();
        if (csrfToken) {
            formData.append('csrf_token', csrfToken);
        }
        
        return new Promise((resolve, reject) => {
            const xhr = new XMLHttpRequest();
            
            xhr.upload.addEventListener('progress', (e) => {
                if (onProgress && e.lengthComputable) {
                    const percentComplete = (e.loaded / e.total) * 100;
                    onProgress(percentComplete);
                }
            });
            
            xhr.addEventListener('load', async () => {
                try {
                    if (xhr.status >= 200 && xhr.status < 300) {
                        const response = JSON.parse(xhr.responseText);
                        resolve(response);
                    } else {
                        const error = JSON.parse(xhr.responseText);
                        reject(new ApiError(error.message, xhr.status));
                    }
                } catch (error) {
                    reject(new ApiError('Invalid response format', xhr.status));
                }
            });
            
            xhr.addEventListener('error', () => {
                reject(new ApiError('Upload failed', 0));
            });
            
            xhr.addEventListener('timeout', () => {
                reject(new ApiError('Upload timeout', 408));
            });
            
            xhr.timeout = this.timeout;
            xhr.open('POST', `${this.baseURL}${endpoint}`);
            xhr.send(formData);
        });
    }
    
    // WebSocket helper for real-time updates
    static createWebSocket(endpoint, options = {}) {
        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        const wsUrl = `${protocol}//${window.location.host}/ws${endpoint}`;
        
        const ws = new WebSocket(wsUrl);
        
        ws.addEventListener('open', () => {
            console.log('WebSocket connected:', endpoint);
            if (options.onOpen) options.onOpen();
        });
        
        ws.addEventListener('message', (event) => {
            try {
                const data = JSON.parse(event.data);
                if (options.onMessage) options.onMessage(data);
            } catch (error) {
                console.error('WebSocket message parse error:', error);
            }
        });
        
        ws.addEventListener('close', () => {
            console.log('WebSocket disconnected:', endpoint);
            if (options.onClose) options.onClose();
        });
        
        ws.addEventListener('error', (error) => {
            console.error('WebSocket error:', error);
            if (options.onError) options.onError(error);
        });
        
        return ws;
    }
}

// Custom error class for API errors
class ApiError extends Error {
    constructor(message, status = 0, errorCode = null) {
        super(message);
        this.name = 'ApiError';
        this.status = status;
        this.errorCode = errorCode;
    }
    
    toString() {
        return `${this.name}: ${this.message} (Status: ${this.status})`;
    }
}

// Rate limiting helper
class RateLimiter {
    constructor(maxRequests = 10, timeWindow = 60000) { // 10 requests per minute
        this.maxRequests = maxRequests;
        this.timeWindow = timeWindow;
        this.requests = [];
    }
    
    canMakeRequest() {
        const now = Date.now();
        
        // Remove old requests outside the time window
        this.requests = this.requests.filter(time => now - time < this.timeWindow);
        
        return this.requests.length < this.maxRequests;
    }
    
    addRequest() {
        this.requests.push(Date.now());
    }
}

// Global rate limiter instance
const globalRateLimiter = new RateLimiter();

// Intercept all API requests for rate limiting
const originalRequest = ApiClient.request;
ApiClient.request = async function(method, endpoint, data, options) {
    if (!globalRateLimiter.canMakeRequest()) {
        throw new ApiError('Rate limit exceeded', 429);
    }
    
    globalRateLimiter.addRequest();
    return originalRequest.call(this, method, endpoint, data, options);
};

// Export for global use
window.ApiClient = ApiClient;
window.ApiError = ApiError;