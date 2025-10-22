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

# Make sure Redis is running before checking modules
if ! systemctl is-active --quiet redis-server; then
    echo "Starting Redis..."
    systemctl start redis-server 2>/dev/null || systemctl start redis 2>/dev/null || true
    sleep 2
fi

# Check if Redis is responding
if redis-cli ping > /dev/null 2>&1; then
    # Check for TimeSeries module
    if redis-cli MODULE LIST 2>/dev/null | grep -q "timeseries"; then
        echo "✓ Redis TimeSeries module is installed"
        REDIS_TS_INSTALLED=1
    else
        echo "⚠ Redis TimeSeries module not found"
        echo ""
        echo "TimeSeries module is required for historical metrics graphs."
        echo ""
        echo "Installation options:"
        echo "  1. Install Redis from official repo (includes TimeSeries)"
        echo "  2. Use Redis Stack Docker container"
        echo ""
        read -p "Install Redis with TimeSeries? (y/n): " -n 1 -r
        echo ""

        if [[ $REPLY =~ ^[Yy]$ ]]; then
            echo "Installing Redis from official repository..."

            # Add Redis repository
            curl -fsSL https://packages.redis.io/gpg | gpg --dearmor -o /usr/share/keyrings/redis-archive-keyring.gpg
            echo "deb [signed-by=/usr/share/keyrings/redis-archive-keyring.gpg] https://packages.redis.io/deb $(lsb_release -cs) main" | tee /etc/apt/sources.list.d/redis.list

            # Install/upgrade Redis
            apt-get update -qq
            apt-get install -y redis-server

            # Restart Redis
            systemctl restart redis-server
            sleep 2

            # Check again
            if redis-cli MODULE LIST 2>/dev/null | grep -q "timeseries"; then
                echo "✓ Redis TimeSeries module installed successfully"
                REDIS_TS_INSTALLED=1
            else
                echo "⚠ TimeSeries module still not available"
                echo "  Try Docker alternative: docker run -d -p 6379:6379 redis/redis-stack-server"
            fi
        else
            echo "Continuing without TimeSeries support..."
            echo "(Metrics will be collected but historical graphs won't work)"
        fi
    fi
else
    echo "⚠ Cannot connect to Redis"
    echo "  Please ensure Redis is installed and running"
    echo "  Install with: sudo apt-get install redis-server"
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

# Stop the service if it's running (for upgrades)
if systemctl is-active --quiet metrics-collector; then
    echo "Stopping existing metrics-collector service..."
    systemctl stop metrics-collector
    sleep 1
fi

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
    echo "  Historical data will not be stored (graphs won't work)"
    echo "  To install:"
    echo "    1. Add Redis repo and upgrade:"
    echo "       curl -fsSL https://packages.redis.io/gpg | sudo gpg --dearmor -o /usr/share/keyrings/redis-archive-keyring.gpg"
    echo "       echo 'deb [signed-by=/usr/share/keyrings/redis-archive-keyring.gpg] https://packages.redis.io/deb \$(lsb_release -cs) main' | sudo tee /etc/apt/sources.list.d/redis.list"
    echo "       sudo apt update && sudo apt install redis-server"
    echo "    2. Or use Docker:"
    echo "       docker run -d -p 6379:6379 --restart unless-stopped redis/redis-stack-server"
    echo ""
fi

echo "Happy monitoring! 🚀"
