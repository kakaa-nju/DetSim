#!/bin/bash
# 测试 NCursesUI 集成

cd /home/kaguya/code/detsim

# 显示帮助信息
echo "=== detsim NCursesUI Test ==="
echo "1. Run with UI (interactive)"
echo "2. Run with auto mode"
echo "3. Run with log file"
echo "q. Quit"

read -p "Select: " choice

case $choice in
    1)
        echo "Running with UI..."
        ./tracer -f examples/raft/raft.json
        ;;
    2)
        echo "Running with auto mode..."
        timeout 10 ./tracer -a -f examples/raft/raft.json
        ;;
    3)
        echo "Running with log file..."
        timeout 10 ./tracer -a -f examples/raft/raft.json -l /tmp/tracer_test.log
        echo "Log saved to /tmp/tracer_test.log"
        ;;
    q)
        exit 0
        ;;
esac
