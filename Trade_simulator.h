#ifndef CONFIG_H
#define CONFIG_H

// WebSocket Configuration
#define CONFIG_HOST "gomarket-cpp.goquant.io"
#define CONFIG_PORT "443"
#define CONFIG_PATH "/ws/l2-orderbook/okx/BTC-USDT-SWAP"

// Exchange Configuration
#define CONFIG_EXCHANGE "OKX"
#define CONFIG_ASSET "BTC-USDT-SWAP"

// Default Parameters
#define CONFIG_DEFAULT_QUANTITY 100.0
#define CONFIG_DEFAULT_VOLATILITY 0.02
#define CONFIG_DEFAULT_FEE_TIER 0.001

// Performance Configuration
#define CONFIG_MAX_HISTORY 1000
#define CONFIG_RETRY_INTERVAL 5
#define CONFIG_PING_INTERVAL 20
#define CONFIG_MAX_LATENCY 100

// Logging Configuration
#define LOG_FILE "simulator.log"

#endif#pragma once
