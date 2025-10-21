<?php
// api/transports.php - Transport API endpoints

require_once dirname(__DIR__) . '/config/config.php';
require_once dirname(__DIR__) . '/services/rist-service.php';

// Handle preflight requests
if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') {
    setCORSHeaders();
    exit;
}

// Set headers
setCORSHeaders();

$ristService = new RistService();
$method = $_SERVER['REQUEST_METHOD'];
$path_info = $_SERVER['PATH_INFO'] ?? '';
$path_parts = explode('/', trim($path_info, '/'));

try {
    switch ($method) {
        case 'GET':
            handleGetRequest($ristService, $path_parts);
            break;
            
        case 'POST':
            handlePostRequest($ristService, $path_parts);
            break;
            
        case 'PUT':
            handlePutRequest($ristService, $path_parts);
            break;
            
        case 'DELETE':
            handleDeleteRequest($ristService, $path_parts);
            break;
            
        default:
            errorResponse('Method not allowed', 405);
    }
} catch (Exception $e) {
    logMessage('ERROR', 'API Error: ' . $e->getMessage(), [
        'method' => $method,
        'path' => $path_info,
        'ip' => getCurrentUserIP()
    ]);
    errorResponse($e->getMessage(), 500);
}

function handleGetRequest($ristService, $path_parts) {
    if (empty($path_parts[0])) {
        // GET /api/transports - List all transports
        $transports = $ristService->getTransports();
        
        // Add status information
        foreach ($transports as &$transport) {
            $status = $ristService->getTransportStatus($transport['id']);
            $transport['runtime_status'] = $status;
        }
        
        successResponse($transports);
        
    } elseif (count($path_parts) === 1) {
        // GET /api/transports/{id} - Get specific transport
        $transport = $ristService->getTransport($path_parts[0]);
        if (!$transport) {
            errorResponse('Transport not found', 404);
        }
        
        $transport['runtime_status'] = $ristService->getTransportStatus($path_parts[0]);
        successResponse($transport);
        
    } elseif (count($path_parts) === 2) {
        $transport_id = $path_parts[0];
        $action = $path_parts[1];
        
        switch ($action) {
            case 'status':
                // GET /api/transports/{id}/status
                $status = $ristService->getTransportStatus($transport_id);
                if (!$status) {
                    errorResponse('Transport not found', 404);
                }
                successResponse($status);
                break;
                
            case 'metrics':
                // GET /api/transports/{id}/metrics
                $metrics = $ristService->getPrometheusMetrics($transport_id);
                successResponse($metrics);
                break;
                
            case 'receivers':
                // GET /api/transports/{id}/receivers
                $receivers = $ristService->getReceivers($transport_id);
                successResponse($receivers);
                break;
                
            case 'logs':
                // GET /api/transports/{id}/logs
                $logs = getTransportLogs($transport_id);
                successResponse($logs);
                break;
                
            default:
                errorResponse('Invalid endpoint', 404);
        }
    } else {
        errorResponse('Invalid endpoint', 404);
    }
}

function handlePostRequest($ristService, $path_parts) {
    if (empty($path_parts[0])) {
        // POST /api/transports - Create new transport
        $input = json_decode(file_get_contents('php://input'), true);
        
        if (!$input) {
            errorResponse('Invalid JSON input', 400);
        }
        
        // Validate CSRF token
       // if (!isset($input['csrf_token']) || !validateCSRFToken($input['csrf_token'])) {
       //     errorResponse('Invalid CSRF token', 403);
      //  }
        
        $transport = $ristService->createTransport($input);
        successResponse($transport, 'Transport created successfully');
        
    } elseif (count($path_parts) === 2) {
        $transport_id = $path_parts[0];
        $action = $path_parts[1];
        
        switch ($action) {
            case 'start':
                // POST /api/transports/{id}/start
                $ristService->startTransport($transport_id);
                successResponse(null, 'Transport started successfully');
                break;
                
            case 'stop':
                // POST /api/transports/{id}/stop
                $ristService->stopTransport($transport_id);
                successResponse(null, 'Transport stopped successfully');
                break;
                
            case 'restart':
                // POST /api/transports/{id}/restart
                $ristService->restartTransport($transport_id);
                successResponse(null, 'Transport restarted successfully');
                break;
                
            default:
                errorResponse('Invalid endpoint', 404);
        }
    } else {
        errorResponse('Invalid endpoint', 404);
    }
}

function handlePutRequest($ristService, $path_parts) {
    if (count($path_parts) === 1) {
        // PUT /api/transports/{id} - Update transport
        $transport_id = $path_parts[0];
        $input = json_decode(file_get_contents('php://input'), true);
        
        if (!$input) {
            errorResponse('Invalid JSON input', 400);
        }
        
        // Validate CSRF token
        if (!isset($input['csrf_token']) || !validateCSRFToken($input['csrf_token'])) {
            errorResponse('Invalid CSRF token', 403);
        }
        
        $transport = $ristService->updateTransport($transport_id, $input);
        successResponse($transport, 'Transport updated successfully');
        
    } else {
        errorResponse('Invalid endpoint', 404);
    }
}

function handleDeleteRequest($ristService, $path_parts) {
    if (count($path_parts) === 1) {
        // DELETE /api/transports/{id} - Delete transport
        $transport_id = $path_parts[0];
        
        // Get CSRF token from headers
        $csrf_token = $_SERVER['HTTP_X_CSRF_TOKEN'] ?? null;
        if (!$csrf_token || !validateCSRFToken($csrf_token)) {
            errorResponse('Invalid CSRF token', 403);
        }
        
        $ristService->deleteTransport($transport_id);
        successResponse(null, 'Transport deleted successfully');
        
    } else {
        errorResponse('Invalid endpoint', 404);
    }
}

function getTransportLogs($transport_id, $lines = 100) {
    $log_file = LOGS_DIR . "/transport_{$transport_id}.log";
    
    if (!file_exists($log_file)) {
        return [];
    }
    
    $command = "tail -n {$lines} " . escapeshellarg($log_file);
    $output = shell_exec($command);
    
    if (!$output) {
        return [];
    }
    
    $lines = explode("\n", trim($output));
    $logs = [];
    
    foreach ($lines as $line) {
        if (empty($line)) continue;
        
        // Parse log line format: [timestamp] [level] message
        if (preg_match('/^\[([^\]]+)\] \[([^\]]+)\] (.+)$/', $line, $matches)) {
            $logs[] = [
                'timestamp' => $matches[1],
                'level' => $matches[2],
                'message' => $matches[3]
            ];
        } else {
            $logs[] = [
                'timestamp' => date('Y-m-d H:i:s'),
                'level' => 'INFO',
                'message' => $line
            ];
        }
    }
    
    return array_reverse($logs); // Most recent first
}
?>