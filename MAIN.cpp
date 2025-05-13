// Include necessary headers
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <nlohmann/json.hpp>
#include <cmath>
#include <gtest/gtest.h> // For unit tests

// Configuration
#include "config.h" // Create this file for configuration parameters

// Order Book Data Structure
struct OrderBook {
    std::string symbol;
    std::vector<std::pair<double, double>> asks;
    std::vector<std::pair<double, double>> bids;
    std::chrono::system_clock::time_point timestamp;
};

// Trade Simulation Results
struct SimulationResults {
    double slippage = 0.0;
    double fees = 0.0;
    double marketImpact = 0.0;
    double netCost = 0.0;
    double makerTakerRatio = 0.0;
    double latency = 0.0;
};

// Global Variables with Mutex
std::deque<OrderBook> orderBookHistory;
std::mutex orderBookMutex;
SimulationResults currentResults;
std::mutex resultsMutex;
bool shouldStop = false;
std::condition_variable cv;
std::mutex cvMutex;

// Logging
class Logger {
public:
    static void Log(const std::string& message, const std::string& level = "INFO") {
        std::ofstream logFile(LOG_FILE, std::ios::app);
        auto now = std::chrono::system_clock::now();
        auto now_c = std::chrono::system_clock::to_time_t(now);
        logFile << "[" << level << "] [" << std::ctime(&now_c) << "] " << message << std::endl;
    }
};

// Exception Handling Utility
class ExceptionHandler {
public:
    static void HandleException(const std::exception& e, const std::string& context) {
        Logger::Log(context + ": " + e.what(), "ERROR");
        // Additional exception handling logic can be added here
    }
};

// WebSocket Handler
class WebSocketHandler {
public:
    WebSocketHandler(boost::asio::io_context& ioc)
        : resolver_(ioc), ws_(ioc), lastPing_(std::chrono::steady_clock::now()) {
    }

    void Connect() {
        try {
            // Resolve the domain name
            auto const results = resolver_.resolve(
                CONFIG_HOST, CONFIG_PORT);

            // Make the connection on the IP address we get from a lookup
            beast::get_lowest_layer(ws_).connect(results);

            // Perform the WebSocket handshake
            ws_.handshake(CONFIG_HOST, CONFIG_PATH);

            Logger::Log("WebSocket connection established successfully.");

            // Start the asynchronous read loop
            StartAsyncRead();

        }
        catch (const std::exception& e) {
            ExceptionHandler::HandleException(e, "WebSocket connection error");
            RetryConnection();
        }
    }

    void StartAsyncRead() {
        ws_.async_read(
            buffer_,
            [this](beast::error_code ec, std::size_t bytes_transferred) {
                if (ec) {
                    if (ec != websocket::error::closed) {
                        ExceptionHandler::HandleException(
                            beast::system_error(ec), "Async read error");
                    }
                    return;
                }

                // Process the received data
                ProcessData(buffer_.data());

                // Continue reading
                StartAsyncRead();
            });
    }

    void ProcessData(beast::multi_buffer const& buffer) {
        try {
            // Convert the buffer to a string
            auto data = buffer.data();
            std::string json_str(data.begin(), data.end());

            // Validate and parse JSON data
            if (!ValidateJson(json_str)) {
                Logger::Log("Invalid JSON data received.", "WARNING");
                return;
            }

            auto json = nlohmann::json::parse(json_str);

            // Create OrderBook object
            OrderBook book;
            book.symbol = json["symbol"];
            book.timestamp = std::chrono::system_clock::now();

            // Parse asks
            for (const auto& ask : json["asks"]) {
                book.asks.emplace_back(ask[0].get<double>(), ask[1].get<double>());
            }

            // Parse bids
            for (const auto& bid : json["bids"]) {
                book.bids.emplace_back(bid[0].get<double>(), bid[1].get<double>());
            }

            // Add to history with mutex protection
            {
                std::lock_guard<std::mutex> lock(orderBookMutex);
                if (orderBookHistory.size() >= CONFIG_MAX_HISTORY) {
                    orderBookHistory.pop_front();
                }
                orderBookHistory.push_back(book);
            }

            // Notify waiting threads
            {
                std::lock_guard<std::mutex> lock(cvMutex);
                cv.notify_all();
            }

        }
        catch (const std::exception& e) {
            ExceptionHandler::HandleException(e, "Data processing error");
        }
    }

    void RetryConnection() {
        std::this_thread::sleep_for(std::chrono::seconds(CONFIG_RETRY_INTERVAL));
        try {
            resolver_.clear();
            ws_.next_layer().close();
            Connect();
        }
        catch (const std::exception& e) {
            ExceptionHandler::HandleException(e, "Connection retry failed");
            RetryConnection();
        }
    }

    void Write(const beast::multi_buffer& buffer) {
        try {
            ws_.write(buffer.data());
        }
        catch (const beast::system_error& e) {
            ExceptionHandler::HandleException(
                beast::system_error(e), "WebSocket write error");
        }
    }

    void Close() {
        try {
            ws_.close(websocket::close_code::normal);
        }
        catch (const std::exception& e) {
            ExceptionHandler::HandleException(e, "Close error");
        }
    }

    void Ping() {
        auto now = std::chrono::steady_clock::now();
        if (now - lastPing_ > std::chrono::seconds(CONFIG_PING_INTERVAL)) {
            try {
                ws_.ping("heartbeat");
                lastPing_ = now;
            }
            catch (const std::exception& e) {
                ExceptionHandler::HandleException(e, "Ping error");
            }
        }
    }

private:
    bool ValidateJson(const std::string& json_str) {
        try {
            auto json = nlohmann::json::parse(json_str);
            if (!json.contains("symbol") || !json.contains("asks") || !json.contains("bids")) {
                return false;
            }
            return true;
        }
        catch (...) {
            return false;
        }
    }

    tcp::resolver resolver_;
    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buffer_;
    std::chrono::steady_clock::time_point lastPing_;
};

// Trade Simulator
class TradeSimulator {
public:
    SimulationResults SimulateTrade(double quantity, double volatility, double feeTier) {
        try {
            ValidateInputs(quantity, volatility, feeTier);

            SimulationResults results;
            OrderBook currentBook;

            {
                std::lock_guard<std::mutex> lock(orderBookMutex);
                if (!orderBookHistory.empty()) {
                    currentBook = orderBookHistory.back();
                }
            }

            if (currentBook.bids.empty() || currentBook.asks.empty()) {
                return results;
            }

            // Measure latency
            auto start = std::chrono::high_resolution_clock::now();

            // Calculate slippage
            results.slippage = CalculateSlippage(quantity, currentBook);

            // Calculate fees
            results.fees = quantity * feeTier;

            // Calculate market impact (Almgren-Chriss model)
            results.marketImpact = CalculateMarketImpact(quantity, volatility);

            // Calculate maker/taker ratio
            results.makerTakerRatio = PredictMakerTakerRatio(quantity, volatility);

            // Calculate net cost
            results.netCost = results.slippage + results.fees + results.marketImpact;

            // Measure latency
            auto end = std::chrono::high_resolution_clock::now();
            results.latency = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

            return results;

        }
        catch (const std::exception& e) {
            ExceptionHandler::HandleException(e, "Trade simulation error");
            return SimulationResults();
        }
    }

private:
    void ValidateInputs(double quantity, double volatility, double feeTier) {
        if (quantity <= 0) throw std::invalid_argument("Quantity must be positive");
        if (volatility < 0) throw std::invalid_argument("Volatility cannot be negative");
        if (feeTier < 0 || feeTier > 1) throw std::invalid_argument("Fee tier must be between 0 and 1");
    }

    double CalculateSlippage(double orderQty, const OrderBook& book) {
        double filled = 0;
        double cost = 0;
        double initialPrice = book.bids[0].first;

        for (const auto& ask : book.asks) {
            double price = ask.first;
            double size = ask.second;
            double take = std::min(orderQty - filled, size);

            cost += take * price;
            filled += take;

            if (filled >= orderQty) break;
        }

        return (cost / orderQty) - initialPrice;
    }

    double CalculateMarketImpact(double orderQty, double volatility) {
        // Simplified Almgren-Chriss model
        double eta = 0.01; // Temporary market impact coefficient
        double gamma = 0.0001; // Permanent market impact coefficient
        double timeHorizon = 1.0; // Execution time in seconds

        return eta * orderQty + gamma * orderQty * orderQty + volatility * sqrt(orderQty) / sqrt(timeHorizon);
    }

    double PredictMakerTakerRatio(double orderQty, double volatility) {
        // Simplified logistic regression model
        return 1 / (1 + exp(-(0.005 * orderQty - 0.1 * volatility + 2)));
    }
};

// UI Component
class TradeSimulatorUI {
public:
    void Render() {
        try {
            system("clear");

            std::cout << "GoQuant Trade Simulator" << std::endl;
            std::cout << "----------------------" << std::endl;
            std::cout << "Exchange: " << CONFIG_EXCHANGE << std::endl;
            std::cout << "Asset: " << CONFIG_ASSET << std::endl;

            std::cout << "\nInput Parameters:" << std::endl;
            std::cout << "Order Type: Market" << std::endl;
            std::cout << "Quantity: " << CONFIG_DEFAULT_QUANTITY << " USD" << std::endl;
            std::cout << "Volatility: " << CONFIG_DEFAULT_VOLATILITY << std::endl;
            std::cout << "Fee Tier: " << CONFIG_DEFAULT_FEE_TIER * 100 << "%" << std::endl;

            SimulationResults results;
            {
                std::lock_guard<std::mutex> lock(resultsMutex);
                results = currentResults;
            }

            std::cout << "\nOutput Parameters:" << std::endl;
            std::cout << "Expected Slippage: " << results.slippage << std::endl;
            std::cout << "Expected Fees: " << results.fees << std::endl;
            std::cout << "Market Impact: " << results.marketImpact << std::endl;
            std::cout << "Net Cost: " << results.netCost << std::endl;
            std::cout << "Maker/Taker Ratio: " << results.makerTakerRatio << std::endl;
            std::cout << "Latency: " << results.latency << " ms" << std::endl;

            if (results.latency > CONFIG_MAX_LATENCY) {
                std::cout << "\nWarning: High latency detected!" << std::endl;
            }

            std::cout << "\nPress Ctrl+C to exit..." << std::endl;

        }
        catch (const std::exception& e) {
            ExceptionHandler::HandleException(e, "UI rendering error");
        }
    }
};

// Simulation Worker Thread
void SimulationWorker() {
    TradeSimulator simulator;
    SimulationResults results;

    while (!shouldStop) {
        std::unique_lock<std::mutex> lock(cvMutex);
        cv.wait(lock, [] { return !orderBookHistory.empty() || shouldStop; });

        if (shouldStop) return;

        try {
            // Simulate trade
            results = simulator.SimulateTrade(
                CONFIG_DEFAULT_QUANTITY,
                CONFIG_DEFAULT_VOLATILITY,
                CONFIG_DEFAULT_FEE_TIER
            );

            // Update results
            {
                std::lock_guard<std::mutex> lock(resultsMutex);
                currentResults = results;
            }

        }
        catch (const std::exception& e) {
            ExceptionHandler::HandleException(e, "Simulation worker error");
        }
    }
}

// Unit Tests
TEST(TradeSimulatorTest, SlippageCalculation) {
    TradeSimulator simulator;
    OrderBook book;
    book.bids.push_back({ 100.0, 10.0 });
    book.asks.push_back({ 101.0, 5.0 });
    book.asks.push_back({ 102.0, 10.0 });

    std::lock_guard<std::mutex> lock(orderBookMutex);
    orderBookHistory.push_back(book);

    SimulationResults results = simulator.SimulateTrade(7.0, 0.01, 0.001);
    EXPECT_NEAR(results.slippage, 1.0, 0.001);
}

TEST(TradeSimulatorTest, MarketImpactCalculation) {
    TradeSimulator simulator;
    double impact = simulator.CalculateMarketImpact(100.0, 0.02);
    EXPECT_GT(impact, 0.0);
}

// Main Function with Proper Shutdown
int main() {
    try {
        // Run unit tests
        testing::InitGoogleTest();
        RUN_ALL_TESTS();

        boost::asio::io_context ioc;
        WebSocketHandler wsHandler(ioc);

        // Register signal handler for proper shutdown
        std::signal(SIGINT, [](int) {
            shouldStop = true;
            });

        // Start WebSocket connection in a separate thread
        std::thread wsThread([&ioc, &wsHandler]() {
            wsHandler.Connect();
            ioc.run();
            });

        // Start simulation worker thread
        std::thread simulationThread(SimulationWorker);

        // UI thread
        TradeSimulatorUI ui;
        while (!shouldStop) {
            ui.Render();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        // Cleanup
        shouldStop = true;
        cv.notify_all();

        wsHandler.Close();
        wsThread.join();
        simulationThread.join();

    }
    catch (const std::exception& e) {
        ExceptionHandler::HandleException(e, "Main exception");
        return 1;
    }

    return 0;
}