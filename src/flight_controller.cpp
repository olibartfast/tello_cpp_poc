#include <amqpcpp.h>
#include <amqpcpp/libuv.h>
#include <iostream>
#include <memory>
#include <vector>
#include <queue>
#include <chrono>
#include <thread>
#include <cmath>

class FlightController {
public:
    enum class ConnectionState { DISCONNECTED, CONNECTING, CONNECTED };

    FlightController(std::string rabbitmq_host, int rabbitmq_port)
        : loop_(create_loop()), handler_(loop_.get()), conn_state_(ConnectionState::DISCONNECTED),
          response_received_(false), reconnect_attempts_(0), shutdown_(false) {
        connect_to_rabbitmq(rabbitmq_host, rabbitmq_port);
        declare_queues();
    }

    ~FlightController() {
        if (conn_) {
            std::cout << "Closing RabbitMQ connection..." << std::endl;
            conn_->close();
            uv_run(loop_.get(), UV_RUN_ONCE);
        }
    }

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

            if (reconnect_attempts_ >= 5) {
                std::cerr << "Maximum reconnection attempts reached. Exiting..." << std::endl;
                throw std::runtime_error("Failed to reconnect to RabbitMQ after 5 attempts");
            }

            int delay = std::min(16, static_cast<int>(std::pow(2, reconnect_attempts_)));
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

    void publish_command(const std::string_view& cmd) {
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

    bool run() {
        const std::vector<std::string_view> commands = {
            "takeoff",
            "forward 10", "cw 90",
            "forward 10", "cw 90",
            "forward 10", "cw 90",
            "forward 10", "cw 90",
            "land"
        };

        for (const auto& cmd : commands) {
            int retries = 3; // Allow up to 3 retries per command
            bool command_success = false;

            while (retries > 0 && !command_success) {
                publish_command(cmd);

                response_received_ = false;
                auto start_time = std::chrono::steady_clock::now();
                while (!response_received_) {
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
                    if (elapsed > 5) {
                        std::cerr << "Timeout waiting for response to command: " << cmd << std::endl;
                        break;
                    }
                    uv_run(loop_.get(), UV_RUN_ONCE);
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }

                if (response_received_) {
                    if (last_response_ == "ok") {
                        command_success = true;
                    } else {
                        std::cerr << "Command " << cmd << " failed with response: " << last_response_ << ". Retries left: " << retries - 1 << std::endl;
                        retries--;
                        if (retries > 0) {
                            std::cout << "Retrying command: " << cmd << std::endl;
                            std::this_thread::sleep_for(std::chrono::seconds(2)); // Delay before retry
                            continue;
                        } else {
                            std::cerr << "Max retries reached for command: " << cmd << std::endl;
                            return false;
                        }
                    }
                } else {
                    retries--;
                    if (retries > 0) {
                        std::cout << "No response, retrying command: " << cmd << ". Retries left: " << retries << std::endl;
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                        continue;
                    } else {
                        std::cerr << "Max retries reached for command: " << cmd << " due to no response" << std::endl;
                        return false;
                    }
                }
            }

            // Add a 2-second delay between commands to allow the drone to stabilize
            if (command_success) {
                std::cout << "Waiting 2 seconds before next command..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }

        std::cout << "All commands processed successfully" << std::endl;
        return true;
    }

    void shutdown() {
        shutdown_ = true;
        if (conn_) {
            std::cout << "Initiating shutdown of RabbitMQ connection..." << std::endl;
            conn_->close();
            uv_run(loop_.get(), UV_RUN_ONCE);
        }
    }

    void run_loop() {
        uv_run(loop_.get(), UV_RUN_DEFAULT);
    }

private:
    struct LoopDeleter {
        void operator()(uv_loop_t* loop) const {
            if (loop) {
                uv_loop_close(loop);
                delete loop;
            }
        }
    };

    static auto create_loop() -> std::unique_ptr<uv_loop_t, LoopDeleter> {
        auto* loop = new uv_loop_t;
        if (int result = uv_loop_init(loop); result != 0) {
            delete loop;
            throw std::runtime_error("Failed to initialize uv_loop: " + std::string(uv_strerror(result)));
        }
        return std::unique_ptr<uv_loop_t, LoopDeleter>(loop);
    }

    std::unique_ptr<uv_loop_t, LoopDeleter> loop_;
    AMQP::LibUvHandler handler_;
    std::unique_ptr<AMQP::TcpConnection> conn_;
    std::unique_ptr<AMQP::TcpChannel> channel_;
    ConnectionState conn_state_;
    bool response_received_;
    std::string last_response_;
    int reconnect_attempts_;
    bool shutdown_;
    std::queue<std::string> command_queue_;
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