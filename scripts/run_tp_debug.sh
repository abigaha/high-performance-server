#!/bin/bash
timeout 180 bin/qps_qps_thread_pool > /tmp/qps_tp_debug.log 2>&1
echo "EXIT=$?"
