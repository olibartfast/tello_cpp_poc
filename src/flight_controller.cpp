#include <amqpcpp.h>
#include <amqpcpp/libuv.h>
#include <iostream>
#include <memory>
#include <vector>
#include <chrono>
#include <thread>

class FlightController {
public:
    FlightController(std::string rabbitmq_host, int rabbitmq_port)
        : loop_(create_loop()), handler_(loop_.get()) {
        AMQP::Address address(std::move(rabbitmq_host), rabbitmq_port, AMQP::Login("guest", "guest"), "/");
        conn_ = std::make_unique<AMQP::TcpConnection>(&handler_, address);
        channel_ = std::make_unique<AMQP::TcpChannel>(conn_.get());

        channel_->declareQueue("tello_commands", AMQP::durable)
            .onSuccess([]() {
                std::cout << "Queue declared successfully" << std::endl;
            })
            .onError([](const char* message) {
                std::cerr << "Queue declare error: " << message << std::endl;
            });

        std::cout << "RabbitMQ connected" << std::endl;
    }

    void run() {
        const std::vector<std::string_view> commands = {
            "takeoff",
            "forward 50", "cw 90",
            "forward 50", "cw 90",
            "forward 50", "cw 90",
            "forward 50", "cw 90",
            "land"
        };

        for (const auto& cmd : commands) {
            AMQP::Envelope envelope(cmd.data(), cmd.size());
            envelope.setDeliveryMode(2); // Persistent
            channel_->publish("", "tello_commands", envelope);
            std::cout << "Published command: " << cmd << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(2));
            uv_run(loop_.get(), UV_RUN_ONCE);
        }

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
};

int main() {
    try {
        FlightController controller("localhost", 5672);
        controller.run();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}