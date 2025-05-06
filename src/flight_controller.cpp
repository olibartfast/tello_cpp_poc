#include <amqpcpp.h>
#include <amqpcpp/libuv.h>
#include <iostream>
#include <memory>
#include <vector>
#include <queue>
#include <chrono>
#include <thread>
#include <cmath>
#include <string_view>

// Configuration struct for all constants, defined outside FlightController
struct FlightControllerConfig {
    // Timeouts (in seconds)
    int takeoff_timeout = 1; // Time to wait for takeoff response
    int default_timeout = 1; // Default timeout for other commands
    int reconnect_delay_max = 16; // Max delay between reconnect attempts
    int takeoff_completion_delay = 1; // Increased to allow takeoff stabilization
    int command_interval = 2; // Interval between commands

    // Retry limits
    int max_reconnect_attempts = 5; // Max RabbitMQ reconnect attempts
    int max_command_retries = 3; // Max retries per command
    int max_takeoff_attempts = 2; // Max takeoff attempts

    // Drone parameters
    int min_battery_level = 20; // Minimum battery percentage
    int min_height_after_takeoff = 2; // Minimum height in decimeters (20 cm)
    int min_distance = 20; // Minimum distance in centimeters
    int max_distance = 500; // Maximum distance in centimeters
    int min_angle = 1; // Minimum angle in degrees
    int max_angle = 360; // Maximum angle in degrees

    // Flight pattern
    int square_side_distance = 20; // Distance for each side of square in centimeters
    int square_turn_angle = 90; // Turn angle for square pattern in degrees
};

class FlightController {
public:
    enum class ConnectionState { DISCONNECTED, CONNECTING, CONNECTED };

    // Constructor with optional configuration
    FlightController(std::string rabbitmq_host, int rabbitmq_port, const FlightControllerConfig& config = FlightControllerConfig())
        : config_(config), loop_(create_loop()), handler_(loop_.get()),
          conn_state_(ConnectionState::DISCONNECTED), response_received_(false),
          reconnect_attempts_(0), shutdown_(false) {
        connect_to_rabbitmq(rabbitmq_host, rabbitmq_port);
        declare_queues();
    }

    // Destructor to clean up RabbitMQ connection
    ~FlightController() {
        if (conn_) {
            std::cout << "Closing RabbitMQ connection..." << std::endl;
            conn_->close();
            uv_run(loop_.get(), UV_RUN_ONCE);
        }
    }

    // Connect to RabbitMQ server
    void connect_to_rabbitmq(const std::string& host, int rabbitmq_port) {
        if (conn_state_ == ConnectionState::CONNECTED) {
            std::cout << "Already connected to RabbitMQ" << std::endl;
            return;
        }

        conn_state_ = ConnectionState::CONNECTING;
        std::cout << "Attempting to connect to RabbitMQ at " << host << ":" << rabbitmq_port << "..." << std::endl;
        AMQP::Address address(host, rabbitmq_port, AMQP::Login("tello_user", "tello_password"), "/", false);
        try {
            conn_ = std::make_unique<AMQP::TcpConnection>(&handler_, address);
        } catch (const std::exception& e) {
            std::cerr << "Failed to create TcpConnection: " << e.what() << std::endl;
            conn_state_ = ConnectionState::DISCONNECTED;
            return;
        }

        channel_ = std::make_unique<AMQP::TcpChannel>(conn_.get());

        channel_->onError([this, host, rabbitmq_port](const char* message) {
            if (shutdown_) {
                std::cout << "Channel error during shutdown: " << message << std::endl;
                return;
            }
            std::cerr << "Channel error: " << message << ". Attempt " << reconnect_attempts_ + 1 << " to reconnect..." << std::endl;
            conn_state_ = ConnectionState::DISCONNECTED;

            if (conn_) {
                conn_->close();
                channel_.reset();
                conn_.reset();
            }

            if (reconnect_attempts_ >= config_.max_reconnect_attempts) {
                std::cerr << "Maximum reconnection attempts reached. Exiting..." << std::endl;
                throw std::runtime_error("Failed to reconnect to RabbitMQ after " + std::to_string(config_.max_reconnect_attempts) + " attempts");
            }

            int delay = std::min(config_.reconnect_delay_max, static_cast<int>(std::pow(2, reconnect_attempts_)));
            std::cout << "Waiting " << delay << " seconds before reconnecting..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(delay));
            reconnect_attempts_++;

            connect_to_rabbitmq(host, rabbitmq_port);
            if (conn_state_ == ConnectionState::CONNECTED) {
                declare_queues();
            }
        });

        channel_->onReady([this]() {
            std::cout << "Channel is ready" << std::endl;
            conn_state_ = ConnectionState::CONNECTED;
            reconnect_attempts_ = 0;
            retry_queued_commands();
        });

        std::cout << "RabbitMQ connection initiated" << std::endl;
    }

    // Declare RabbitMQ queues for commands and responses
    void declare_queues() {
        if (!channel_) {
            std::cerr << "Cannot declare queues: channel is null" << std::endl;
            return;
        }

        channel_->declareQueue("tello_commands", AMQP::durable)
            .onSuccess([]() {
                std::cout << "Command queue declared successfully" << std::endl;
            })
            .onError([](const char* message) {
                std::cerr << "Queue declare error: " << message << std::endl;
            });

        channel_->declareQueue("tello_responses", AMQP::durable)
            .onSuccess([this]() {
                std::cout << "Response queue declared successfully" << std::endl;
                if (channel_) {
                    channel_->consume("tello_responses", AMQP::noack)
                        .onReceived([this](const AMQP::Message& message, uint64_t, bool) {
                            std::string_view response(message.body(), message.bodySize());
                            std::cout << "Received response: " << response << std::endl;
                            last_response_ = std::string(response);
                            response_received_ = true;
                        })
                        .onError([](const char* message) {
                            std::cerr << "Consume error: " << message << std::endl;
                        });
                }
            })
            .onError([](const char* message) {
                std::cerr << "Response queue declare error: " << message << std::endl;
            });
    }

    // Validate drone commands
    bool validate_command(const std::string_view& cmd) {
        size_t space_pos = cmd.find(' ');
        if (space_pos == std::string_view::npos) {
            return true;
        }

        std::string_view command = cmd.substr(0, space_pos);
        std::string_view param = cmd.substr(space_pos + 1);
        int value = 0;
        try {
            value = std::stoi(std::string(param));
        } catch (const std::exception& e) {
            std::cerr << "Invalid parameter in command: " << cmd << std::endl;
            return false;
        }

        if (command == "forward" || command == "back" || command == "left" || command == "right" || command == "up" || command == "down") {
            if (value < config_.min_distance || value > config_.max_distance) {
                std::cerr << "Distance parameter for " << command << " must be between " << config_.min_distance
                          << " and " << config_.max_distance << " cm, got: " << value << std::endl;
                return false;
            }
        } else if (command == "cw" || command == "ccw") {
            if (value < config_.min_angle || value > config_.max_angle) {
                std::cerr << "Angle parameter for " << command << " must be between " << config_.min_angle
                          << " and " << config_.max_angle << " degrees, got: " << value << std::endl;
                return false;
            }
        }

        return true;
    }

    // Wait for RabbitMQ connection to be established
    bool wait_for_connection(int timeout_seconds) {
        auto start_time = std::chrono::steady_clock::now();
        while (conn_state_ != ConnectionState::CONNECTED) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
            if (elapsed > timeout_seconds) {
                std::cerr << "Timeout waiting for RabbitMQ connection" << std::endl;
                return false;
            }
            uv_run(loop_.get(), UV_RUN_ONCE);
            std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Reduced for faster response
        }
        return true;
    }

    // Issue and confirm land command
    bool issue_land_command() {
        if (!wait_for_connection(config_.default_timeout)) {
            std::cerr << "Cannot issue land command: RabbitMQ not connected" << std::endl;
            return false;
        }

        publish_command("land");
        response_received_ = false;
        last_response_.clear(); // Clear previous response
        auto start_time = std::chrono::steady_clock::now();
        while (!response_received_) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
            if (elapsed > config_.default_timeout) {
                std::cerr << "Timeout waiting for land response" << std::endl;
                return false;
            }
            uv_run(loop_.get(), UV_RUN_NOWAIT); // Non-blocking to process responses
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        std::cout << "Land response: " << last_response_ << std::endl;
        if (last_response_ == "ok" || last_response_ == "error") { // Treat error as valid (already landed)
            std::cout << "Drone landed successfully or already on ground" << std::endl;
            return true;
        } else {
            std::cerr << "Failed to confirm landing: " << last_response_ << std::endl;
            return false;
        }
    }

    // Perform pre-flight checks (battery, takeoff, height)
    bool pre_flight_check() {
        // Query battery level
        if (!wait_for_connection(config_.default_timeout)) {
            std::cerr << "Cannot query battery: RabbitMQ not connected" << std::endl;
            return false;
        }

        publish_command("battery?");
        response_received_ = false;
        last_response_.clear();
        auto start_time = std::chrono::steady_clock::now();
        while (!response_received_) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
            if (elapsed > config_.default_timeout) {
                std::cerr << "Timeout waiting for battery response" << std::endl;
                return false;
            }
            uv_run(loop_.get(), UV_RUN_NOWAIT);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        int battery_level = 0;
        try {
            battery_level = std::stoi(last_response_);
        } catch (const std::exception& e) {
            std::cerr << "Invalid battery response: " << last_response_ << std::endl;
            return false;
        }
        std::cout << "Battery level: " << battery_level << "%" << std::endl;
        if (battery_level < config_.min_battery_level) {
            std::cerr << "Battery level too low for flight: " << battery_level << "%" << std::endl;
            return false;
        }

        // Perform takeoff with retry
        int takeoff_attempts = config_.max_takeoff_attempts;
        bool takeoff_success = false;
        while (takeoff_attempts > 0 && !takeoff_success) {
            if (!wait_for_connection(config_.takeoff_timeout)) {
                std::cerr << "Cannot issue takeoff: RabbitMQ not connected" << std::endl;
                return false;
            }

            publish_command("takeoff");
            response_received_ = false;
            last_response_.clear();
            start_time = std::chrono::steady_clock::now();
            while (!response_received_) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
                if (elapsed > config_.takeoff_timeout) {
                    std::cerr << "Timeout waiting for takeoff response. Connection state: " << static_cast<int>(conn_state_) << std::endl;
                    break;
                }
                uv_run(loop_.get(), UV_RUN_NOWAIT);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            if (response_received_ && last_response_ == "ok") {
                takeoff_success = true;
            } else {
                std::cerr << "Takeoff attempt " << (config_.max_takeoff_attempts - takeoff_attempts + 1) << " failed with response: " << last_response_ << std::endl;
                takeoff_attempts--;
                if (takeoff_attempts > 0) {
                    std::cout << "Retrying takeoff..." << std::endl;
                    issue_land_command();
                    std::this_thread::sleep_for(std::chrono::seconds(config_.command_interval));
                }
            }
        }

        if (!takeoff_success) {
            std::cerr << "Takeoff failed after retries" << std::endl;
            issue_land_command();
            return false;
        }

        // Wait for takeoff to complete
        std::cout << "Waiting " << config_.takeoff_completion_delay << " seconds for takeoff to complete..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(config_.takeoff_completion_delay));

        // Query height to confirm takeoff
        if (!wait_for_connection(config_.default_timeout)) {
            std::cerr << "Cannot query height: RabbitMQ not connected" << std::endl;
            issue_land_command();
            return false;
        }

        publish_command("height?");
        response_received_ = false;
        last_response_.clear();
        start_time = std::chrono::steady_clock::now();
        while (!response_received_) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
            if (elapsed > config_.default_timeout) {
                std::cerr << "Timeout waiting for height response" << std::endl;
                issue_land_command();
                return false;
            }
            uv_run(loop_.get(), UV_RUN_NOWAIT);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        int height = 0;
        try {
            height = std::stoi(last_response_);
        } catch (const std::exception& e) {
            std::cerr << "Invalid height response: " << last_response_ << std::endl;
            issue_land_command();
            return false;
        }
        std::cout << "Height after takeoff: " << height << " dm" << std::endl;
        if (height < config_.min_height_after_takeoff) {
            std::cerr << "Height too low after takeoff: " << height << " dm" << std::endl;
            issue_land_command();
            return false;
        }

        return true;
    }

    // Publish a command to RabbitMQ, queuing if connection is not ready
    void publish_command(const std::string_view& cmd) {
        if (!validate_command(cmd)) {
            std::cerr << "Skipping invalid command: " << cmd << std::endl;
            last_response_ = "invalid command";
            response_received_ = true;
            return;
        }

        AMQP::Envelope envelope(cmd.data(), cmd.size());
        envelope.setDeliveryMode(2);

        if (conn_state_ != ConnectionState::CONNECTED || !channel_) {
            std::cout << "Connection not ready, queuing command: " << cmd << std::endl;
            command_queue_.push(std::string(cmd));
            return;
        }

        bool success = channel_->publish("", "tello_commands", envelope);
        if (!success) {
            std::cerr << "Failed to publish command: " << cmd << ", queuing for retry..." << std::endl;
            command_queue_.push(std::string(cmd));
        } else {
            std::cout << "Published command: " << cmd << std::endl;
        }
    }

    // Retry queued commands when connection is restored
    void retry_queued_commands() {
        while (!command_queue_.empty() && conn_state_ == ConnectionState::CONNECTED && channel_) {
            auto cmd = command_queue_.front();
            AMQP::Envelope envelope(cmd.data(), cmd.size());
            envelope.setDeliveryMode(2);
            bool success = channel_->publish("", "tello_commands", envelope);
            if (success) {
                std::cout << "Successfully retried command: " << cmd << std::endl;
                command_queue_.pop();
            } else {
                std::cerr << "Retry failed for command: " << cmd << ", keeping in queue..." << std::endl;
                break;
            }
        }
    }

    // Execute the flight pattern
    bool run() {
        // Perform pre-flight checks
        if (!pre_flight_check()) {
            std::cerr << "Pre-flight check failed, aborting flight pattern" << std::endl;
            issue_land_command();
            return false;
        }

        // Define flight pattern using config values
        std::vector<std::string> commands = {
            "forward " + std::to_string(config_.square_side_distance),
            "cw " + std::to_string(config_.square_turn_angle),
            "forward " + std::to_string(config_.square_side_distance),
            "cw " + std::to_string(config_.square_turn_angle),
            "forward " + std::to_string(config_.square_side_distance),
            "cw " + std::to_string(config_.square_turn_angle),
            "forward " + std::to_string(config_.square_side_distance),
            "cw " + std::to_string(config_.square_turn_angle),
            "land"
        };

        for (const auto& cmd : commands) {
            int retries = config_.max_command_retries;
            bool command_success = false;

            while (retries > 0 && !command_success) {
                if (!wait_for_connection(config_.default_timeout)) {
                    std::cerr << "Cannot execute command " << cmd << ": RabbitMQ not connected" << std::endl;
                    issue_land_command();
                    return false;
                }

                publish_command(cmd);
                response_received_ = false;
                last_response_.clear();
                auto start_time = std::chrono::steady_clock::now();
                while (!response_received_) {
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
                    if (elapsed > config_.default_timeout) {
                        std::cerr << "Timeout waiting for response to command: " << cmd << std::endl;
                        break;
                    }
                    uv_run(loop_.get(), UV_RUN_NOWAIT);
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }

                if (response_received_) {
                    if (last_response_ == "ok" || (cmd == "land" && last_response_ == "error")) {
                        command_success = true;
                    } else if (last_response_ == "out of range" || last_response_ == "invalid command") {
                        std::cerr << "Unrecoverable error for command " << cmd << ": " << last_response_ << std::endl;
                        issue_land_command();
                        return false;
                    } else {
                        std::cerr << "Command " << cmd << " failed with response: " << last_response_ << ". Retries left: " << retries - 1 << std::endl;
                        retries--;
                        if (retries > 0) {
                            std::cout << "Retrying command: " << cmd << std::endl;
                            std::this_thread::sleep_for(std::chrono::seconds(config_.command_interval));
                            continue;
                        } else {
                            std::cerr << "Max retries reached for command: " << cmd << std::endl;
                            issue_land_command();
                            return false;
                        }
                    }
                } else {
                    retries--;
                    if (retries > 0) {
                        std::cout << "No response, retrying command: " << cmd << ". Retries left: " << retries << std::endl;
                        std::this_thread::sleep_for(std::chrono::seconds(config_.command_interval));
                        continue;
                    } else {
                        std::cerr << "Max retries reached for command: " << cmd << " due to no response" << std::endl;
                        issue_land_command();
                        return false;
                    }
                }
            }

            if (command_success) {
                std::cout << "Waiting " << config_.command_interval << " seconds before next command..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(config_.command_interval));
            }
        }

        std::cout << "All commands processed successfully" << std::endl;
        return true;
    }

    // Shutdown RabbitMQ connection
    void shutdown() {
        shutdown_ = true;
        if (conn_) {
            std::cout << "Initiating shutdown of RabbitMQ connection..." << std::endl;
            conn_->close();
            uv_run(loop_.get(), UV_RUN_ONCE);
        }
    }

    // Run the event loop
    void run_loop() {
        uv_run(loop_.get(), UV_RUN_DEFAULT);
    }

private:
    // Custom deleter for uv_loop_t
    struct LoopDeleter {
        void operator()(uv_loop_t* loop) const {
            if (loop) {
                uv_loop_close(loop);
                delete loop;
            }
        }
    };

    // Create a new uv_loop_t with custom deleter
    static auto create_loop() -> std::unique_ptr<uv_loop_t, LoopDeleter> {
        auto* loop = new uv_loop_t;
        if (int result = uv_loop_init(loop); result != 0) {
            delete loop;
            throw std::runtime_error("Failed to initialize uv_loop: " + std::string(uv_strerror(result)));
        }
        return std::unique_ptr<uv_loop_t, LoopDeleter>(loop);
    }

    FlightControllerConfig config_;
    std::unique_ptr<uv_loop_t, LoopDeleter> loop_;
    AMQP::LibUvHandler handler_;
    std::unique_ptr<AMQP::TcpConnection> conn_;
    std::unique_ptr<AMQP::TcpChannel> channel_;
    ConnectionState conn_state_;
    bool response_received_;
    std::string last_response_;
    int reconnect_attempts_;
    bool shutdown_;
    std::queue<std::string> command_queue_; // Queue for commands when connection is not ready
};

int main() {
    try {
        FlightController controller("localhost", 5672);
        if (controller.run()) {
            std::cout << "Flight pattern completed successfully" << std::endl;
        } else {
            std::cerr << "Flight pattern failed" << std::endl;
        }
        controller.shutdown();
        controller.run_loop();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}