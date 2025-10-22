# Quick Start Guide - RIST Metrics Collector

## Installation (30 seconds)

```bash
cd /home/user/Satellite-Hybrid-Management-System/worker
sudo ./install.sh
```

The script will:
- ✓ Install dependencies (libcurl, hiredis, redis)
- ✓ Compile the C program
- ✓ Install to /usr/local/bin
- ✓ Set up systemd service
- ✓ Optionally start Redis Stack (with TimeSeries)

## Manual Installation

If you prefer manual installation:

```bash
# 1. Install dependencies
sudo apt-get update
sudo apt-get install -y build-essential libcurl4-openssl-dev libhiredis-dev redis-server

# 2. Install Redis TimeSeries (choose one option)

# Option A: Redis Stack (Docker - RECOMMENDED)
docker run -d -p 6379:6379 --name redis-stack redis/redis-stack-server:latest

# Option B: System package (if available)
sudo apt-get install redis-timeseries

# 3. Build and install
make
sudo make install

# 4. Start service
sudo systemctl enable metrics-collector
sudo systemctl start metrics-collector
```

## Verify Installation

```bash
# Check service is running
sudo systemctl status metrics-collector

# View live logs
sudo journalctl -u metrics-collector -f

# Test Redis connection
redis-cli ping
# Should output: PONG

# Check TimeSeries module
redis-cli MODULE LIST
# Should show "timeseries"
```

## Expected Output

When running, you should see:

```
[transport_123] Worker started (port 9102)
[transport_123] Collected 2 peers (1.93 Mbps total)
```

## View Collected Metrics

```bash
# Connect to Redis
redis-cli

# List all keys
KEYS metrics:*

# Example output:
# metrics:transport_123:peer_5:bandwidth
# metrics:transport_123:peer_5:quality
# metrics:transport_123:peer_5:rtt
# metrics:transport_123:peer_5:packet_loss

# Get last hour of bandwidth data
TS.RANGE metrics:transport_123:peer_5:bandwidth <timestamp_from> <timestamp_to>

# Get recent values (last 5 minutes)
TS.RANGE metrics:transport_123:peer_5:bandwidth $(expr $(date +%s) - 300)000 $(date +%s)000
```

## Troubleshooting

### Service Won't Start

```bash
# Check logs
sudo journalctl -u metrics-collector -n 100

# Common issues:
# 1. Redis not running
sudo systemctl status redis
sudo systemctl start redis

# 2. Config file missing
ls -la /var/www/html/rist-monitor/config/transports.json

# 3. Permissions
sudo chown root:root /usr/local/bin/metrics-collector
sudo chmod +x /usr/local/bin/metrics-collector
```

### No Metrics Appearing

```bash
# 1. Check if transports are actually running
ps aux | grep ristsender

# 2. Test metrics endpoint manually
curl http://127.0.0.1:9101/metrics

# 3. Check collector is connecting
sudo journalctl -u metrics-collector -f

# Should see:
# [transport_123] Collected 2 peers
```

### Redis TimeSeries Not Working

```bash
# Check if module loaded
redis-cli MODULE LIST

# If empty, install Redis Stack instead
docker run -d -p 6379:6379 redis/redis-stack-server:latest

# Restart collector
sudo systemctl restart metrics-collector
```

## Next Steps

Once metrics are being collected, you can:

1. **View in Frontend**: Metrics update every 5 seconds in the web UI
2. **Query Historical Data**: Use Redis CLI or PHP API
3. **Build Graphs**: Implement Chart.js graphs using the time-series data
4. **Set Alerts**: Monitor metrics and trigger alerts on anomalies

## Performance

- **CPU**: <1% for 10 transports
- **Memory**: ~10 MB
- **Storage**: ~1 MB per peer per day (in Redis)
- **Update Frequency**: 5 seconds

## Useful Commands

```bash
# Start/Stop
sudo systemctl start metrics-collector
sudo systemctl stop metrics-collector
sudo systemctl restart metrics-collector

# Enable/Disable auto-start
sudo systemctl enable metrics-collector
sudo systemctl disable metrics-collector

# Logs
sudo journalctl -u metrics-collector -f          # Follow
sudo journalctl -u metrics-collector -n 100      # Last 100 lines
sudo journalctl -u metrics-collector --since "1 hour ago"

# Test run (foreground)
/usr/local/bin/metrics-collector

# Uninstall
cd /home/user/Satellite-Hybrid-Management-System/worker
sudo make uninstall
```

## Support

- GitHub Issues: https://github.com/caritechsolutions/Satellite-Hybrid-Management-System/issues
- Documentation: See README.md in this directory
