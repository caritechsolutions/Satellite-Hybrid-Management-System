<?php
// config/config.php - Main configuration file

// Application Configuration
define('APP_NAME', 'RIST Monitor');
define('APP_VERSION', '1.0.0');
define('APP_ROOT', dirname(__DIR__));

// File Paths
define('CONFIG_DIR', APP_ROOT . '/config');
define('DATA_DIR', APP_ROOT . '/data');
define('LOGS_DIR', '/var/log/rist-monitor');

// Data Files
define('TRANSPORTS_FILE', CONFIG_DIR . '/transports.json');
define('SATELLITES_FILE', CONFIG_DIR . '/satellites.json');
define('RECEIVERS_FILE', DATA_DIR . '/receivers.json');

// System Configuration
define('RIST_SENDER_BINARY', '/usr/local/bin/ristsender');
define('RIST_RECEIVER_BINARY', '/usr/local/bin/ristreceiver');
define('SYSTEMCTL_BINARY', '/usr/bin/systemctl');

// Prometheus Configuration
define('PROMETHEUS_ENDPOINT', 'http://localhost:9090');
define('PROMETHEUS_METRICS_PORT', 8080);

// Network Configuration
define('DEFAULT_BUFFER_SIZE', 8000);
define('DEFAULT_WEIGHT_SATELLITE', 0);
define('DEFAULT_WEIGHT_RECOVERY', 1000);

// Logging Configuration
define('LOG_LEVEL', 'INFO'); // DEBUG, INFO, WARNING, ERROR
define('LOG_FILE', LOGS_DIR . '/rist-monitor.log');
define('MAX_LOG_SIZE', 10 * 1024 * 1024); // 10MB
define('MAX_LOG_FILES', 5);

// Security Configuration
// define('ALLOWED_IPS', ['127.0.0.1', '::1']); // Add your admin IPs
define('ALLOWED_IPS', ['127.0.0.1', '::1', '192.168.110.18']); // Add your admin IPs
define('CSRF_TOKEN_NAME', 'csrf_token');
define('SESSION_TIMEOUT', 3600); // 1 hour

// API Configuration
define('API_TIMEOUT', 30); // seconds
define('API_RATE_LIMIT', 100); // requests per minute
define('API_VERSION', 'v1');

// Default Satellite Database
$default_satellites = [
    [
        'id' => 'eutelsat_65w',
        'name' => 'Eutelsat 65W',
        'position' => '65.0W',
        'frequency_min' => 10950,
        'frequency_max' => 12750,
        'status' => 'active'
    ],
    [
        'id' => 'ses_17',
        'name' => 'SES-17',
        'position' => '67.1W',
        'frequency_min' => 10950,
        'frequency_max' => 12750,
        'status' => 'active'
    ],
    [
        'id' => 'astra_19e',
        'name' => 'Astra 19.2°E',
        'position' => '19.2E',
        'frequency_min' => 10700,
        'frequency_max' => 12750,
        'status' => 'active'
    ],
    [
        'id' => 'intelsat_34',
        'name' => 'Intelsat 34',
        'position' => '55.5W',
        'frequency_min' => 10950,
        'frequency_max' => 12750,
        'status' => 'active'
    ]
];

// Error Handling
function handleError($errno, $errstr, $errfile, $errline) {
    $error_message = "Error: [$errno] $errstr in $errfile on line $errline";
    error_log($error_message, 3, LOG_FILE);
    
    if ($errno === E_ERROR || $errno === E_PARSE || $errno === E_CORE_ERROR) {
        die('A critical error occurred. Please check the logs.');
    }
    
    return true;
}

function handleException($exception) {
    $error_message = "Uncaught exception: " . $exception->getMessage() . 
                    " in " . $exception->getFile() . 
                    " on line " . $exception->getLine();
    error_log($error_message, 3, LOG_FILE);
    die('An unexpected error occurred. Please check the logs.');
}

set_error_handler('handleError');
set_exception_handler('handleException');

// Utility Functions
function getConfigPath($filename) {
    return CONFIG_DIR . '/' . $filename;
}

function getDataPath($filename) {
    if (!is_dir(DATA_DIR)) {
        mkdir(DATA_DIR, 0755, true);
    }
    return DATA_DIR . '/' . $filename;
}

function logMessage($level, $message, $context = []) {
    $timestamp = date('Y-m-d H:i:s');
    $log_entry = "[{$timestamp}] [{$level}] {$message}";
    
    if (!empty($context)) {
        $log_entry .= ' ' . json_encode($context);
    }
    
    $log_entry .= PHP_EOL;
    
    // Ensure log directory exists
    if (!is_dir(LOGS_DIR)) {
        mkdir(LOGS_DIR, 0755, true);
    }
    
    // Rotate logs if necessary
    if (file_exists(LOG_FILE) && filesize(LOG_FILE) > MAX_LOG_SIZE) {
        for ($i = MAX_LOG_FILES - 1; $i > 0; $i--) {
            $old_file = LOG_FILE . '.' . $i;
            $new_file = LOG_FILE . '.' . ($i + 1);
            if (file_exists($old_file)) {
                rename($old_file, $new_file);
            }
        }
        rename(LOG_FILE, LOG_FILE . '.1');
    }
    
    file_put_contents(LOG_FILE, $log_entry, FILE_APPEND | LOCK_EX);
}

function validateIP($ip) {
    return filter_var($ip, FILTER_VALIDATE_IP) !== false;
}

function validatePort($port) {
    return is_numeric($port) && $port >= 1 && $port <= 65535;
}

function validateURL($url) {
    return filter_var($url, FILTER_VALIDATE_URL) !== false;
}

function sanitizeInput($input) {
    return htmlspecialchars(trim($input), ENT_QUOTES, 'UTF-8');
}

function generateCSRFToken() {
    if (session_status() === PHP_SESSION_NONE) {
        session_start();
    }
    
    if (!isset($_SESSION[CSRF_TOKEN_NAME])) {
        $_SESSION[CSRF_TOKEN_NAME] = bin2hex(random_bytes(32));
    }
    
    return $_SESSION[CSRF_TOKEN_NAME];
}

function validateCSRFToken($token) {
    if (session_status() === PHP_SESSION_NONE) {
        session_start();
    }
    
    return isset($_SESSION[CSRF_TOKEN_NAME]) && 
           hash_equals($_SESSION[CSRF_TOKEN_NAME], $token);
}

function isAllowedIP($ip) {
    if (empty(ALLOWED_IPS)) {
        return true; // No IP restrictions
    }
    
    return in_array($ip, ALLOWED_IPS);
}

function getCurrentUserIP() {
    if (!empty($_SERVER['HTTP_CLIENT_IP'])) {
        return $_SERVER['HTTP_CLIENT_IP'];
    } elseif (!empty($_SERVER['HTTP_X_FORWARDED_FOR'])) {
        return $_SERVER['HTTP_X_FORWARDED_FOR'];
    } else {
        return $_SERVER['REMOTE_ADDR'];
    }
}

// Initialize application
function initializeApp() {
    // Check if required directories exist
    $required_dirs = [CONFIG_DIR, DATA_DIR, LOGS_DIR];
    
    foreach ($required_dirs as $dir) {
        if (!is_dir($dir)) {
            mkdir($dir, 0755, true);
        }
    }
    
    // Create default configuration files if they don't exist
    if (!file_exists(SATELLITES_FILE)) {
        file_put_contents(SATELLITES_FILE, json_encode([
            'satellites' => $GLOBALS['default_satellites']
        ], JSON_PRETTY_PRINT));
    }
    
    if (!file_exists(TRANSPORTS_FILE)) {
        file_put_contents(TRANSPORTS_FILE, json_encode([
            'transports' => []
        ], JSON_PRETTY_PRINT));
    }
    
    if (!file_exists(RECEIVERS_FILE)) {
        file_put_contents(RECEIVERS_FILE, json_encode([
            'receivers' => [],
            'last_updated' => date('c')
        ], JSON_PRETTY_PRINT));
    }
    
    // Set timezone
    date_default_timezone_set('UTC');
    
    // Start session if not already started
    if (session_status() === PHP_SESSION_NONE) {
        session_start();
    }
    
    // Security check
    /*
    $client_ip = getCurrentUserIP();
    echo $_client_ip;
    if (!isAllowedIP($client_ip)) {
        logMessage('WARNING', 'Access denied for IP: ' . $client_ip);
        http_response_code(403);
        die('Access denied');
    }
    */
    logMessage('INFO', 'Application initialized for IP: ' . $client_ip);
}

// CORS Headers for API
function setCORSHeaders() {
    header('Access-Control-Allow-Origin: *');
    header('Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS');
    header('Access-Control-Allow-Headers: Content-Type, Authorization, X-Requested-With');
    header('Content-Type: application/json; charset=utf-8');
}

// Response helper functions
function jsonResponse($data, $status_code = 200) {
    http_response_code($status_code);
    setCORSHeaders();
    echo json_encode($data, JSON_PRETTY_PRINT);
    exit;
}

function errorResponse($message, $status_code = 400, $error_code = null) {
    $response = [
        'error' => true,
        'message' => $message
    ];
    
    if ($error_code !== null) {
        $response['error_code'] = $error_code;
    }
    
    jsonResponse($response, $status_code);
}

function successResponse($data = null, $message = 'Success') {
    $response = [
        'error' => false,
        'message' => $message
    ];
    
    if ($data !== null) {
        $response['data'] = $data;
    }
    
    jsonResponse($response);
}

// Initialize the application
initializeApp();
?>