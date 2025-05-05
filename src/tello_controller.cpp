#include "tello.hpp"
#include <amqpcpp.h>
#include <amqpcpp/libuv.h>
#include <iostream>
#include <memory>

class TelloController {
public:
    TelloController(std::string ip, int port, std::string rabbitmq_host, int rabbitmq_port)
        : loop_(create_loop()), handler_(loop_.get()),
          tello_(std::move(ip), port, *loop_) {
        // Connect to Tello
        if (auto result = tello_.connect(); !result) {
            std::cerr << "Failed to connect to Tello" << std::endl;
            throw std::runtime_error("Tello connection failed");
        }

        // Connect to RabbitMQ
        AMQP::Address address(std::move(rabbitmq_host), rabbitmq_port, AMQP::Login("guest", "guest"), "/");
        conn_ = std::make_unique<AMQP::TcpConnection>(&handler_, address);
        channel_ = std::make_unique<AMQP::TcpChannel>(conn_.get());

        channel_->declareQueue("tello_commands", AMQP::durable)
            .onSuccess([this]() {
                channel_->consume("tello_commands", AMQP::noack)
                    .onReceived([this](const AMQP::Message& message, uint64_t, bool) {
                        std::string_view cmd(message.body(), message.bodySize());
                        std::cout << "Received command: " << cmd << std::endl;
                        if (auto response = tello_.send_command(cmd)) {
                            std::cout << "Tello response: " << *response << std::endl;
                        } else {
                            std::cerr << "Failed to send command: " << cmd << std::endl;
                        }
                    })
                    .onError([](const char* message) {
                        std::cerr << "Consume error: " << message << std::endl;
                    });
            })
            .onError([](const char* message) {
                std::cerr << "Queue declare error: " << message << std::endl;
            });

        std::cout << "TelloController started, listening for RabbitMQ commands..." << std::endl;
    }

    void run() {
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
    Tello tello_;
};

int main() {
    try {
        TelloController controller("192.168.10.1", 8889, "localhost", 5672);
        controller.run();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}