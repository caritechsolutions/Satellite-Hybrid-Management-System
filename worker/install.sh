#!/bin/bash

# RIST Metrics Collector Installation Script
# Installs dependencies, compiles, and sets up the systemd service

set -e

echo "╔════════════════════════════════════════════════════╗"
echo "║  RIST Metrics Collector - Installation Script    ║"
echo "╚════════════════════════════════════════════════════╝"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root (use sudo)"
    exit 1
fi

# Detect OS
if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS=$ID
    VERSION=$VERSION_ID
else
    echo "Cannot detect OS. Exiting."
    exit 1
fi

echo "Detected OS: $PRETTY_NAME"
echo ""

# Install dependencies
echo "Step 1: Installing dependencies..."
echo "─────────────────────────────────────"

if [ "$OS" = "ubuntu" ] || [ "$OS" = "debian" ]; then
    apt-get update -qq
    apt-get install -y -qq \
        build-essential \
        libcurl4-openssl-dev \
        libhiredis-dev \
        redis-server \
        git \
        make \
        gcc

    echo "✓ Dependencies installed"
else
    echo "Unsupported OS: $OS"
    echo "Please install manually:"
    echo "  - build-essential"
    echo "  - libcurl4-openssl-dev"
    echo "  - libhiredis-dev"
    echo "  - redis-server"
    exit 1
fi

echo ""

# Check for Redis TimeSeries
echo "Step 2: Checking Redis TimeSeries module..."
echo "─────────────────────────────────────────────"

REDIS_TS_INSTALLED=0

if redis-cli MODULE LIST 2>/dev/null | grep -q "timeseries"; then
    echo "✓ Redis TimeSeries module is installed"
    REDIS_TS_INSTALLED=1
else
    echo "⚠ Redis TimeSeries module not found"
    echo ""
    echo "Options:"
    echo "  1. Install redis-timeseries package (if available)"
    echo "  2. Use Redis Stack Docker container"
    echo ""
    read -p "Install Redis Stack via Docker? (y/n): " -n 1 -r
    echo ""

    if [[ $REPLY =~ ^[Yy]$ ]]; then
        # Check if Docker is installed
        if command -v docker &> /dev/null; then
            echo "Starting Redis Stack container..."
            docker run -d -p 6379:6379 \
                --name redis-stack \
                --restart unless-stopped \
                redis/redis-stack-server:latest

            echo "✓ Redis Stack container started"
            REDIS_TS_INSTALLED=1
        else
            echo "Docker not found. Please install Docker or Redis TimeSeries module manually."
            echo "Continuing anyway... (basic Redis will work, but historical data won't be stored)"
        fi
    else
        echo "Continuing without TimeSeries support..."
        echo "(Metrics will be collected but not stored historically)"
    fi
fi

echo ""

# Build
echo "Step 3: Building metrics collector..."
echo "───────────────────────────────────────"

cd "$(dirname "$0")"

if [ -f "Makefile" ]; then
    make clean 2>/dev/null || true
    make
    echo "✓ Build successful"
else
    echo "Makefile not found. Compiling manually..."
    gcc -Wall -Wextra -O3 -pthread -std=c11 -o metrics-collector \
        metrics-collector.c -lcurl -lhiredis -lm
    echo "✓ Compiled successfully"
fi

echo ""

# Install
echo "Step 4: Installing..."
echo "──────────────────────"

cp metrics-collector /usr/local/bin/
chmod +x /usr/local/bin/metrics-collector
echo "✓ Binary installed to /usr/local/bin/metrics-collector"

cp metrics-collector.service /etc/systemd/system/
systemctl daemon-reload
echo "✓ Systemd service installed"

echo ""

# Test configuration
echo "Step 5: Testing configuration..."
echo "─────────────────────────────────"

CONFIG_FILE="/var/www/html/rist-monitor/config/transports.json"
if [ -f "$CONFIG_FILE" ]; then
    echo "✓ Config file found: $CONFIG_FILE"
else
    echo "⚠ Config file not found: $CONFIG_FILE"
    echo "  Make sure RIST Monitor is installed"
fi

# Test Redis connection
if redis-cli ping > /dev/null 2>&1; then
    echo "✓ Redis is running"
else
    echo "⚠ Redis is not responding"
    echo "  Starting Redis..."
    systemctl start redis
    sleep 2
    if redis-cli ping > /dev/null 2>&1; then
        echo "✓ Redis started"
    else
        echo "✗ Failed to start Redis"
    fi
fi

echo ""

# Enable and start service
echo "Step 6: Starting service..."
echo "───────────────────────────"

read -p "Enable metrics collector to start on boot? (y/n): " -n 1 -r
echo ""

if [[ $REPLY =~ ^[Yy]$ ]]; then
    systemctl enable metrics-collector
    echo "✓ Service enabled for auto-start"
fi

read -p "Start metrics collector now? (y/n): " -n 1 -r
echo ""

if [[ $REPLY =~ ^[Yy]$ ]]; then
    systemctl start metrics-collector
    sleep 2

    if systemctl is-active --quiet metrics-collector; then
        echo "✓ Service started successfully"
    else
        echo "✗ Service failed to start"
        echo "  Check logs with: sudo journalctl -u metrics-collector -n 50"
    fi
fi

echo ""
echo "╔════════════════════════════════════════════════════╗"
echo "║              Installation Complete!                ║"
echo "╚════════════════════════════════════════════════════╝"
echo ""
echo "Useful Commands:"
echo "  Status:   sudo systemctl status metrics-collector"
echo "  Logs:     sudo journalctl -u metrics-collector -f"
echo "  Stop:     sudo systemctl stop metrics-collector"
echo "  Start:    sudo systemctl start metrics-collector"
echo "  Restart:  sudo systemctl restart metrics-collector"
echo ""
echo "Test Redis TimeSeries:"
echo "  redis-cli TS.INFO metrics:transport_id:peer_id:bandwidth"
echo ""

if [ $REDIS_TS_INSTALLED -eq 0 ]; then
    echo "⚠ WARNING: Redis TimeSeries is not installed"
    echo "  Historical data will not be stored"
    echo "  To install:"
    echo "    - Docker: docker run -d -p 6379:6379 redis/redis-stack-server"
    echo "    - Or: sudo apt-get install redis-timeseries"
    echo ""
fi

echo "Happy monitoring! 🚀"
