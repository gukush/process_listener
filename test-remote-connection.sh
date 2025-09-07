#!/bin/bash

# Test script for remote server connection
# Usage: ./test-remote-connection.sh <remote_host>

if [ $# -eq 0 ]; then
    echo "Usage: $0 <remote_host>"
    echo "Example: $0 192.168.1.100"
    exit 1
fi

REMOTE_HOST=$1
echo "Testing connection to remote server: $REMOTE_HOST"

# Test HTTP endpoint (Socket.IO)
echo "Testing HTTP endpoint (port 3000)..."
if curl -s -I "http://$REMOTE_HOST:3000" | grep -q "200 OK"; then
    echo "✓ HTTP endpoint is accessible"
else
    echo "✗ HTTP endpoint is not accessible"
fi

# Test WebSocket endpoint
echo "Testing WebSocket endpoint (port 3001)..."
if timeout 5 bash -c "</dev/tcp/$REMOTE_HOST/3001" 2>/dev/null; then
    echo "✓ WebSocket endpoint is accessible"
else
    echo "✗ WebSocket endpoint is not accessible"
fi

# Test with wscat if available
if command -v wscat &> /dev/null; then
    echo "Testing WebSocket connection with wscat..."
    timeout 10 wscat -c "ws://$REMOTE_HOST:3001/ws-native" --close &
    WSCAT_PID=$!
    sleep 2
    if kill -0 $WSCAT_PID 2>/dev/null; then
        echo "✓ WebSocket connection successful"
        kill $WSCAT_PID 2>/dev/null
    else
        echo "✗ WebSocket connection failed"
    fi
else
    echo "wscat not available, skipping WebSocket connection test"
fi

echo "Test completed. If all tests pass, you can use:"
echo "  ./unified_monitor --mode browser+cpp --server ws://$REMOTE_HOST:3001"

