<?php
// services/rist-service.php - RIST Service Management Class

require_once dirname(__DIR__) . '/config/config.php';

class RistService {
    
    private $transports_file;
    private $satellites_file;
    private $receivers_file;
    
    public function __construct() {
        $this->transports_file = TRANSPORTS_FILE;
        $this->satellites_file = SATELLITES_FILE;
        $this->receivers_file = RECEIVERS_FILE;
    }
    
    // Transport Management
    public function getTransports() {
        if (!file_exists($this->transports_file)) {
            return [];
        }
        
        $data = json_decode(file_get_contents($this->transports_file), true);
        return $data['transports'] ?? [];
    }
    
    public function getTransport($id) {
        $transports = $this->getTransports();
        foreach ($transports as $transport) {
            if ($transport['id'] === $id) {
                return $transport;
            }
        }
        return null;
    }
    
    public function createTransport($data) {
        // Validate input data
        $required_fields = ['name', 'satellite', 'input_url', 'output_urls'];
        foreach ($required_fields as $field) {
            if (!isset($data[$field]) || empty($data[$field])) {
                throw new Exception("Missing required field: {$field}");
            }
        }
        
        // Generate unique ID
        $id = strtolower(preg_replace('/[^a-zA-Z0-9]/', '_', $data['name'])) . '_' . time();
        
        // Validate URLs
        if (!$this->validateInputURL($data['input_url'])) {
            throw new Exception("Invalid input URL format");
        }
        
        foreach ($data['output_urls'] as $url) {
            if (!$this->validateOutputURL($url)) {
                throw new Exception("Invalid output URL format: {$url}");
            }
        }
        
        // Create transport record
        $transport = [
            'id' => $id,
            'name' => sanitizeInput($data['name']),
            'satellite' => sanitizeInput($data['satellite']),
            'input_url' => $data['input_url'],
            'output_urls' => $data['output_urls'],
            'status' => 'stopped',
            'pid' => null,
            'created_at' => date('c'),
            'updated_at' => date('c'),
            'metrics' => [
                'packets_sent' => 0,
                'packets_lost' => 0,
                'bandwidth_mbps' => 0,
                'uptime_seconds' => 0
            ]
        ];
        
        // Save to file
        $transports = $this->getTransports();
        $transports[] = $transport;
        $this->saveTransports($transports);
        
        logMessage('INFO', "Created transport: {$id}", $transport);
        return $transport;
    }
    
    public function updateTransport($id, $data) {
        $transports = $this->getTransports();
        $found = false;
        
        foreach ($transports as &$transport) {
            if ($transport['id'] === $id) {
                // Update allowed fields
                $allowed_fields = ['name', 'satellite', 'input_url', 'output_urls'];
                foreach ($allowed_fields as $field) {
                    if (isset($data[$field])) {
                        if ($field === 'input_url' && !$this->validateInputURL($data[$field])) {
                            throw new Exception("Invalid input URL format");
                        }
                        if ($field === 'output_urls') {
                            foreach ($data[$field] as $url) {
                                if (!$this->validateOutputURL($url)) {
                                    throw new Exception("Invalid output URL format: {$url}");
                                }
                            }
                        }
                        $transport[$field] = $data[$field];
                    }
                }
                $transport['updated_at'] = date('c');
                $found = true;
                break;
            }
        }
        
        if (!$found) {
            throw new Exception("Transport not found: {$id}");
        }
        
        $this->saveTransports($transports);
        logMessage('INFO', "Updated transport: {$id}");
        return $this->getTransport($id);
    }
    
    public function deleteTransport($id) {
        // Stop transport if running
        $this->stopTransport($id);
        
        $transports = $this->getTransports();
        $transports = array_filter($transports, function($transport) use ($id) {
            return $transport['id'] !== $id;
        });
        
        $this->saveTransports(array_values($transports));
        logMessage('INFO', "Deleted transport: {$id}");
        return true;
    }
    
    public function startTransport($id) {
        $transport = $this->getTransport($id);
        if (!$transport) {
            throw new Exception("Transport not found: {$id}");
        }

        // Check if service is already running
        $serviceStatus = $this->getServiceStatus($id);
        if ($serviceStatus === 'active') {
            $this->updateTransportStatus($id, 'running', null);
            throw new Exception("Transport service is already running: {$id}");
        }

        // Generate systemd service file
        $serviceFile = $this->generateSystemdServiceFile($transport);

        // Install the service file
        $serviceName = "rist-transport-{$id}.service";
        $serviceDestination = "/etc/systemd/system/{$serviceName}";

        // Copy service file to systemd directory (requires sudo)
        $installCmd = "sudo cp " . escapeshellarg($serviceFile) . " " . escapeshellarg($serviceDestination);
        exec($installCmd, $output, $return_code);

        if ($return_code !== 0) {
            throw new Exception("Failed to install service file for transport: {$id}");
        }

        // Reload systemd daemon
        exec("sudo systemctl daemon-reload", $output, $return_code);

        // Enable the service (auto-start on reboot)
        exec("sudo systemctl enable {$serviceName}", $output, $return_code);

        // Start the service
        exec("sudo systemctl start {$serviceName} 2>&1", $output, $return_code);

        if ($return_code === 0) {
            // Update transport status
            $this->updateTransportStatus($id, 'running', null);
            logMessage('INFO', "Started transport service: {$id}");
            return true;
        } else {
            $errorMsg = implode("\n", $output);
            logMessage('ERROR', "Failed to start transport service: {$id}", ['error' => $errorMsg]);
            throw new Exception("Failed to start transport service: {$id}. Error: {$errorMsg}");
        }
    }
    
    public function stopTransport($id) {
        $transport = $this->getTransport($id);
        if (!$transport) {
            throw new Exception("Transport not found: {$id}");
        }

        $serviceName = "rist-transport-{$id}.service";

        // Stop the service
        exec("sudo systemctl stop {$serviceName} 2>&1", $output, $return_code);

        // Disable the service (prevent auto-start on reboot)
        exec("sudo systemctl disable {$serviceName} 2>&1", $output2, $return_code2);

        // Update transport status regardless of command result
        $this->updateTransportStatus($id, 'stopped', null);

        // Remove the service file
        $serviceDestination = "/etc/systemd/system/{$serviceName}";
        if (file_exists($serviceDestination)) {
            exec("sudo rm " . escapeshellarg($serviceDestination), $output3, $return_code3);
            exec("sudo systemctl daemon-reload", $output4, $return_code4);
        }

        logMessage('INFO', "Stopped and disabled transport service: {$id}");
        return true;
    }
    
    public function restartTransport($id) {
        $this->stopTransport($id);
        sleep(2); // Wait for clean shutdown
        return $this->startTransport($id);
    }
    
    public function getTransportStatus($id) {
        $transport = $this->getTransport($id);
        if (!$transport) {
            return null;
        }

        // Check systemd service status
        $serviceStatus = $this->getServiceStatus($id);

        // Update transport status based on service status
        if ($serviceStatus === 'active') {
            if ($transport['status'] !== 'running') {
                $this->updateTransportStatus($id, 'running', null);
                $transport['status'] = 'running';
            }
        } else {
            if ($transport['status'] !== 'stopped') {
                $this->updateTransportStatus($id, 'stopped', null);
                $transport['status'] = 'stopped';
            }
        }

        return [
            'id' => $transport['id'],
            'name' => $transport['name'],
            'status' => $transport['status'],
            'service_status' => $serviceStatus,
            'uptime' => $this->getTransportUptime($transport),
            'metrics' => $transport['metrics'] ?? []
        ];
    }
    
    // Satellite Management
    public function getSatellites() {
        if (!file_exists($this->satellites_file)) {
            return [];
        }
        
        $data = json_decode(file_get_contents($this->satellites_file), true);
        return $data['satellites'] ?? [];
    }
    
    public function getSatellite($id) {
        $satellites = $this->getSatellites();
        foreach ($satellites as $satellite) {
            if ($satellite['id'] === $id) {
                return $satellite;
            }
        }
        return null;
    }
    
    // Receiver Management
    public function getReceivers($transport_id = null) {
        if (!file_exists($this->receivers_file)) {
            return [];
        }
        
        $data = json_decode(file_get_contents($this->receivers_file), true);
        $receivers = $data['receivers'] ?? [];
        
        if ($transport_id) {
            $receivers = array_filter($receivers, function($receiver) use ($transport_id) {
                return $receiver['transport_id'] === $transport_id;
            });
        }
        
        return array_values($receivers);
    }
    
    public function updateReceiverData($receivers_data) {
        $data = [
            'receivers' => $receivers_data,
            'last_updated' => date('c')
        ];
        
        file_put_contents($this->receivers_file, json_encode($data, JSON_PRETTY_PRINT));
        return true;
    }
    
    // Metrics and Monitoring
    public function getPrometheusMetrics($transport_id = null) {
        $transport = $transport_id ? $this->getTransport($transport_id) : null;
        
        if (!$transport || $transport['status'] !== 'running') {
            return null;
        }
        
        // Query Prometheus for metrics
        $metrics_url = PROMETHEUS_ENDPOINT . '/api/v1/query';
        
        $queries = [
            'packets_sent' => 'rist_packets_sent_total{instance="' . $transport_id . '"}',
            'packets_lost' => 'rist_packets_lost_total{instance="' . $transport_id . '"}',
            'bandwidth' => 'rist_bandwidth_mbps{instance="' . $transport_id . '"}',
            'rtt' => 'rist_rtt_ms{instance="' . $transport_id . '"}'
        ];
        
        $metrics = [];
        foreach ($queries as $metric => $query) {
            $url = $metrics_url . '?' . http_build_query(['query' => $query]);
            $response = $this->httpGet($url);
            
            if ($response) {
                $data = json_decode($response, true);
                if (isset($data['data']['result'][0]['value'][1])) {
                    $metrics[$metric] = $data['data']['result'][0]['value'][1];
                }
            }
        }
        
        return $metrics;
    }
    
    // Private Methods
    private function saveTransports($transports) {
        $data = ['transports' => $transports];
        file_put_contents($this->transports_file, json_encode($data, JSON_PRETTY_PRINT));
    }
    
    private function updateTransportStatus($id, $status, $pid = null) {
        $transports = $this->getTransports();
        
        foreach ($transports as &$transport) {
            if ($transport['id'] === $id) {
                $transport['status'] = $status;
                $transport['pid'] = $pid;
                $transport['updated_at'] = date('c');
                break;
            }
        }
        
        $this->saveTransports($transports);
    }
    
    private function buildRistCommand($transport) {
        $cmd = RIST_SENDER_BINARY;
        
        // Input URL
        $cmd .= ' -i ' . escapeshellarg($transport['input_url']);
        
        // Output URLs
        $output_urls = implode(',', array_map('escapeshellarg', $transport['output_urls']));
        $cmd .= ' -o ' . $output_urls;
        
        // Add buffer size if not specified in URLs
        foreach ($transport['output_urls'] as $url) {
            if (strpos($url, 'buffer=') === false) {
                $cmd .= ' -b ' . DEFAULT_BUFFER_SIZE;
                break;
            }
        }
        
        // Add metrics export
        $cmd .= ' --metrics-http --metrics-port ' . PROMETHEUS_METRICS_PORT;
        
        // Add logging
        $cmd .= ' --verbose-level 1';
        
        return $cmd;
    }
    
    private function validateInputURL($url) {
        // Basic UDP/RTP URL validation
        $pattern = '/^(udp|rtp):\/\/@?[\d\.]+:\d+$/';
        return preg_match($pattern, $url);
    }
    
    private function validateOutputURL($url) {
        // Basic RIST URL validation
        $pattern = '/^rist:\/\/@?[\d\.]+:\d+(\?.*)?$/';
        return preg_match($pattern, $url);
    }
    
    private function getTransportUptime($transport) {
        if ($transport['status'] !== 'running' || !isset($transport['updated_at'])) {
            return 0;
        }
        
        $start_time = strtotime($transport['updated_at']);
        return time() - $start_time;
    }
    
    private function httpGet($url, $timeout = 10) {
        $ch = curl_init();
        curl_setopt($ch, CURLOPT_URL, $url);
        curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
        curl_setopt($ch, CURLOPT_TIMEOUT, $timeout);
        curl_setopt($ch, CURLOPT_FOLLOWLOCATION, true);
        curl_setopt($ch, CURLOPT_SSL_VERIFYPEER, false);
        
        $response = curl_exec($ch);
        $http_code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
        
        curl_close($ch);
        
        if ($http_code === 200) {
            return $response;
        }
        
        return false;
    }
    
    // System Health Check
    public function getSystemHealth() {
        $transports = $this->getTransports();
        $running_count = 0;
        $stopped_count = 0;
        $error_count = 0;

        foreach ($transports as $transport) {
            $status = $this->getTransportStatus($transport['id']);
            switch ($status['status']) {
                case 'running':
                    $running_count++;
                    break;
                case 'stopped':
                    $stopped_count++;
                    break;
                default:
                    $error_count++;
            }
        }

        return [
            'total_transports' => count($transports),
            'running' => $running_count,
            'stopped' => $stopped_count,
            'errors' => $error_count,
            'cpu_usage' => $this->getCPUUsage(),
            'memory_usage' => $this->getMemoryUsage(),
            'disk_usage' => $this->getDiskUsage()
        ];
    }

    private function getCPUUsage() {
        // Try to get CPU usage from /proc/stat
        if (file_exists('/proc/stat')) {
            $stat1 = file('/proc/stat');
            usleep(100000); // 100ms
            $stat2 = file('/proc/stat');

            $info1 = explode(" ", preg_replace("!cpu +!", "", $stat1[0]));
            $info2 = explode(" ", preg_replace("!cpu +!", "", $stat2[0]));

            $dif = array();
            $dif['user'] = $info2[0] - $info1[0];
            $dif['nice'] = $info2[1] - $info1[1];
            $dif['sys'] = $info2[2] - $info1[2];
            $dif['idle'] = $info2[3] - $info1[3];

            $total = array_sum($dif);
            $cpu = [];
            foreach($dif as $x=>$y) $cpu[$x] = round($y / $total * 100, 1);

            return round(100 - $cpu['idle'], 1);
        }

        // Fallback: use system load average
        $load = sys_getloadavg();
        return round(min($load[0] * 20, 100), 1); // Rough approximation
    }

    private function getMemoryUsage() {
        // Try Linux method first
        if (file_exists('/proc/meminfo')) {
            $meminfo = file_get_contents('/proc/meminfo');
            preg_match('/MemTotal:\s+(\d+)/', $meminfo, $total_match);
            preg_match('/MemAvailable:\s+(\d+)/', $meminfo, $available_match);

            if ($total_match && $available_match) {
                $total = $total_match[1];
                $available = $available_match[1];
                $used = $total - $available;
                return round(($used / $total) * 100, 1);
            }
        }

        // Fallback: try using 'free' command
        $output = @shell_exec('free 2>/dev/null');
        if ($output) {
            $free = (string)trim($output);
            $free_arr = explode("\n", $free);
            if (count($free_arr) > 1) {
                $mem = preg_split('/\s+/', $free_arr[1]);
                if (count($mem) >= 3 && $mem[1] > 0) {
                    return round(($mem[2] / $mem[1]) * 100, 1);
                }
            }
        }

        // Last resort: return 0
        return 0;
    }
    
    private function getDiskUsage() {
        $bytes = disk_free_space(".");
        $si_prefix = array( 'B', 'KB', 'MB', 'GB', 'TB', 'EB', 'ZB', 'YB' );
        $base = 1024;
        $class = min((int)log($bytes , $base) , count($si_prefix) - 1);
        $free_space = sprintf('%1.2f' , $bytes / pow($base,$class)) . ' ' . $si_prefix[$class];

        $total_bytes = disk_total_space(".");
        $used_bytes = $total_bytes - $bytes;
        $usage_percentage = ($used_bytes / $total_bytes) * 100;

        return [
            'free_space' => $free_space,
            'usage_percentage' => round($usage_percentage, 2)
        ];
    }

    // Systemd Service Management
    private function generateSystemdServiceFile($transport) {
        // Build ristsender command
        $senderCmd = $this->buildRistSenderCommand($transport);

        // Build ristreceiver command
        $receiverCmd = $this->buildRistReceiverCommand($transport);

        // Create temporary directory for service files if it doesn't exist
        $tmpDir = '/tmp/rist-services';
        if (!file_exists($tmpDir)) {
            mkdir($tmpDir, 0755, true);
        }

        $serviceFile = "{$tmpDir}/rist-transport-{$transport['id']}.service";

        // Generate systemd service file content
        $serviceContent = <<<EOD
[Unit]
Description=RIST Transport: {$transport['name']}
After=network.target

[Service]
Type=forking
Restart=always
RestartSec=5

# Start ristsender first
ExecStartPre=/bin/bash -c '{$senderCmd} > /dev/null 2>&1 &'
ExecStartPre=/bin/sleep 2

# Start ristreceiver after ristsender is running
ExecStart=/bin/bash -c '{$receiverCmd} > /dev/null 2>&1 &'

# Stop commands
ExecStop=/usr/bin/pkill -f "ristsender.*{$transport['id']}"
ExecStop=/usr/bin/pkill -f "ristreceiver.*{$transport['id']}"

[Install]
WantedBy=multi-user.target
EOD;

        // Write service file
        file_put_contents($serviceFile, $serviceContent);
        chmod($serviceFile, 0644);

        return $serviceFile;
    }

    private function buildRistSenderCommand($transport) {
        $cmd = 'ristsender';

        // Input URL
        $cmd .= ' -i ' . escapeshellarg($transport['input_url']);

        // Output URLs - join with comma
        if (!empty($transport['output_urls'])) {
            $outputUrls = array_map(function($url) {
                return escapeshellarg($url);
            }, $transport['output_urls']);
            $cmd .= ' -o ' . implode(',', $outputUrls);
        }

        // Set verbosity to 3 (reduced from default)
        $cmd .= ' -v 3';

        return $cmd;
    }

    private function buildRistReceiverCommand($transport) {
        // Use the first RIST output URL as the receiver input
        if (empty($transport['output_urls'])) {
            throw new Exception("No output URLs configured for transport: {$transport['id']}");
        }

        // Extract the first RIST URL
        $ristUrl = $transport['output_urls'][0];

        // Parse RIST URL to get host:port (remove parameters)
        if (preg_match('/rist:\/\/@?([\d\.]+:\d+)/', $ristUrl, $matches)) {
            $ristInput = 'rist://' . $matches[1];
        } else {
            throw new Exception("Invalid RIST URL format: {$ristUrl}");
        }

        // Build receiver command
        $cmd = '/root/part7rist/ristreceiver_with_markers';
        $cmd .= ' -i ' . escapeshellarg($ristInput);

        // Output to multicast (example: udp://239.6.6.6:6000)
        // For now, use a default - this should be configurable in the transport settings
        $receiverOutput = 'udp://239.6.6.6:6000';
        $cmd .= ' -o ' . escapeshellarg($receiverOutput);

        // Set verbosity to 3 (reduced from -v 6)
        $cmd .= ' -v 3';

        return $cmd;
    }

    private function getServiceStatus($id) {
        $serviceName = "rist-transport-{$id}.service";

        // Check if service is active
        exec("systemctl is-active {$serviceName} 2>&1", $output, $return_code);

        $status = trim(implode('', $output));

        // Return 'active', 'inactive', 'failed', etc.
        return $status;
    }
}
?>