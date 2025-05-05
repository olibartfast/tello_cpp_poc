#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <uv.h>
#include <memory>

class Tello {
public:
    Tello(std::string ip, int port, uv_loop_t& loop);
    ~Tello() = default; // RAII cleanup via unique_ptr

    std::optional<std::string> connect();
    std::optional<std::string> send_command(std::string_view cmd);

private:
    struct UdpDeleter {
        void operator()(uv_udp_t* udp) const {
            if (udp) {
                uv_udp_recv_stop(udp);
                uv_close(reinterpret_cast<uv_handle_t*>(udp), [](uv_handle_t* handle) {
                    delete reinterpret_cast<uv_udp_t*>(handle);
                });
            }
        }
    };

    std::string ip_;
    int port_;
    uv_loop_t& loop_;
    std::unique_ptr<uv_udp_t, UdpDeleter> udp_socket_;
    std::string last_response_;
    bool response_received_ = false;
};