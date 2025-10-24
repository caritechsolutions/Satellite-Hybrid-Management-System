<?php
// api/metrics-history.php - Historical Metrics API for Redis TimeSeries

require_once dirname(__DIR__) . '/config/config.php';

// Handle preflight requests
if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') {
    setCORSHeaders();
    exit;
}

// Set headers
setCORSHeaders();

try {
    // Connect to Redis
    $redis = new Redis();
    $redis->connect('127.0.0.1', 6379);

    if (!$redis->ping()) {
        throw new Exception('Redis connection failed');
    }

    // Get query parameters
    $transportId = $_GET['transport_id'] ?? null;
    $peerId = $_GET['peer_id'] ?? null;
    $metric = $_GET['metric'] ?? 'bandwidth';
    $timeRange = $_GET['range'] ?? '1h'; // 1h, 6h, 24h

    if (!$transportId || !$peerId) {
        errorResponse('Missing required parameters: transport_id, peer_id', 400);
    }

    // Validate metric
    $validMetrics = ['bandwidth', 'quality', 'rtt', 'packet_loss', 'retry_bandwidth'];
    if (!in_array($metric, $validMetrics)) {
        errorResponse('Invalid metric. Valid options: ' . implode(', ', $validMetrics), 400);
    }

    // Calculate time range
    $now = time() * 1000; // milliseconds
    $ranges = [
        '5m' => 5 * 60 * 1000,
        '15m' => 15 * 60 * 1000,
        '30m' => 30 * 60 * 1000,
        '1h' => 3600 * 1000,
        '6h' => 6 * 3600 * 1000,
        '12h' => 12 * 3600 * 1000,
        '24h' => 24 * 3600 * 1000
    ];

    $timeRangeMs = $ranges[$timeRange] ?? $ranges['1h'];
    $fromTimestamp = $now - $timeRangeMs;

    // Build Redis key
    $key = "metrics:{$transportId}:{$peerId}:{$metric}";

    // Check if key exists
    $exists = $redis->rawCommand('EXISTS', $key);
    if (!$exists) {
        // Return empty data if no historical data yet
        successResponse([
            'transport_id' => $transportId,
            'peer_id' => $peerId,
            'metric' => $metric,
            'range' => $timeRange,
            'data' => [],
            'message' => 'No historical data available yet. Collector may not be running or peer just connected.'
        ]);
    }

    // Query Redis TimeSeries using redis-cli to avoid PhpRedis parsing bug
    // PhpRedis incorrectly parses TS.RANGE decimal values as booleans
    $aggregationInterval = 5000; // 5 seconds (matches collection interval)

    // Build redis-cli command
    if ($timeRangeMs > 3600 * 1000) { // > 1 hour
        $aggregationInterval = 60000;
        $cmd = sprintf(
            'redis-cli TS.RANGE %s %s %s AGGREGATION avg %d 2>/dev/null',
            escapeshellarg($key),
            escapeshellarg((string)$fromTimestamp),
            escapeshellarg((string)$now),
            $aggregationInterval
        );
    } else {
        $cmd = sprintf(
            'redis-cli TS.RANGE %s %s %s 2>/dev/null',
            escapeshellarg($key),
            escapeshellarg((string)$fromTimestamp),
            escapeshellarg((string)$now)
        );
    }

    // Execute and parse output
    $output = shell_exec($cmd);

    // Parse redis-cli output format (simple alternating lines)
    // Line 1: timestamp, Line 2: value, Line 3: timestamp, Line 4: value, ...
    $formattedData = [];
    if ($output) {
        $lines = array_values(array_filter(explode("\n", $output), 'strlen')); // Remove empty lines

        // Process pairs of lines (timestamp, value)
        for ($i = 0; $i < count($lines) - 1; $i += 2) {
            $timestamp = (int)trim($lines[$i]);
            $value = (float)trim($lines[$i + 1]);

            $formattedData[] = [
                'timestamp' => $timestamp,
                'value' => $value
            ];
        }
    }

    // Get metadata
    $info = $redis->rawCommand('TS.INFO', $key);
    $totalSamples = 0;
    if (is_array($info)) {
        for ($i = 0; $i < count($info); $i++) {
            if ($info[$i] === 'totalSamples') {
                $totalSamples = $info[$i + 1] ?? 0;
                break;
            }
        }
    }

    successResponse([
        'transport_id' => $transportId,
        'peer_id' => $peerId,
        'metric' => $metric,
        'range' => $timeRange,
        'from' => $fromTimestamp,
        'to' => $now,
        'total_samples' => $totalSamples,
        'returned_points' => count($formattedData),
        'aggregation_interval_ms' => $aggregationInterval,
        'data' => $formattedData
    ]);

} catch (Exception $e) {
    logMessage('ERROR', 'Metrics history API error: ' . $e->getMessage());
    errorResponse($e->getMessage(), 500);
}

$redis->close();
?>
