#!/bin/bash
#
# Health Check Script for RIST Metrics Collector
# Returns 0 if healthy, 1 if unhealthy
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

HEALTHY=0
UNHEALTHY=1

echo "=== RIST Metrics Collector Health Check ==="
echo

# Check 1: Process is running
echo -n "Checking if collector process is running... "
if pgrep -x "metrics-collector" > /dev/null; then
    echo -e "${GREEN}OK${NC}"
    PID=$(pgrep -x "metrics-collector")
    echo "  PID: $PID"
else
    echo -e "${RED}FAILED${NC}"
    echo "  ERROR: metrics-collector process not found"
    exit $UNHEALTHY
fi

# Check 2: Redis is accessible
echo -n "Checking Redis connection... "
if redis-cli PING > /dev/null 2>&1; then
    echo -e "${GREEN}OK${NC}"
else
    echo -e "${RED}FAILED${NC}"
    echo "  ERROR: Cannot connect to Redis"
    exit $UNHEALTHY
fi

# Check 3: Recent data exists in Redis
echo -n "Checking for recent metrics data... "

# Get all bandwidth metric keys
KEYS=$(redis-cli KEYS "metrics:*:peer_id=*:bandwidth" 2>/dev/null)

if [ -z "$KEYS" ]; then
    echo -e "${YELLOW}WARNING${NC}"
    echo "  No metrics keys found in Redis (collector may have just started)"
    # This is a warning, not a failure
else
    # Check the most recent timestamp across all keys
    MOST_RECENT=0
    for KEY in $KEYS; do
        LAST_TS=$(redis-cli TS.RANGE "$KEY" - + COUNT 1 2>/dev/null | head -1)
        if [ -n "$LAST_TS" ] && [ "$LAST_TS" -gt "$MOST_RECENT" ]; then
            MOST_RECENT=$LAST_TS
        fi
    done

    if [ "$MOST_RECENT" -eq 0 ]; then
        echo -e "${YELLOW}WARNING${NC}"
        echo "  No data points found in any metrics keys"
    else
        # Calculate age of most recent data point
        CURRENT_TIME=$(date +%s%3N)
        AGE_MS=$((CURRENT_TIME - MOST_RECENT))
        AGE_SEC=$((AGE_MS / 1000))

        echo -e "${GREEN}OK${NC}"
        echo "  Most recent data: ${AGE_SEC}s ago"

        # Warn if data is older than 30 seconds
        if [ "$AGE_SEC" -gt 30 ]; then
            echo -e "  ${YELLOW}WARNING: Data is older than 30 seconds${NC}"
            echo "  This may indicate the collector is stalled"
        fi
    fi
fi

# Check 4: Collector can write to Redis (check for errors in logs)
echo -n "Checking recent logs for errors... "
if command -v journalctl > /dev/null 2>&1; then
    ERROR_COUNT=$(journalctl -u metrics-collector --since "5 minutes ago" --no-pager 2>/dev/null | grep -c "ERROR:" || true)
    if [ "$ERROR_COUNT" -gt 0 ]; then
        echo -e "${YELLOW}WARNING${NC}"
        echo "  Found $ERROR_COUNT error(s) in last 5 minutes"
        echo "  Run: journalctl -u metrics-collector -n 50"
    else
        echo -e "${GREEN}OK${NC}"
    fi
else
    echo -e "${YELLOW}SKIPPED${NC} (journalctl not available)"
fi

echo
echo -e "${GREEN}✓ Health check passed${NC}"
exit $HEALTHY
