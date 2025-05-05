#include "tello.hpp"
#include <stdexcept>
#include <iostream>
#include <thread>

Tello::Tello(std::string ip, int port, uv_loop_t& loop)
    : ip_(std::move(ip)), port_(port), loop_(loop) {
    udp_socket_ = std::unique_ptr<uv_udp_t, UdpDeleter>(new uv_udp_t);
    uv_udp_init(&loop_, udp_socket_.get());
    udp_socket_->data = this;

    // Bind to port 8889 to receive command responses
    struct sockaddr_in bind_addr;
    uv_ip4_addr("0.0.0.0", 8889, &bind_addr); // Changed from 0 to 8889
    int result = uv_udp_bind(udp_socket_.get(), reinterpret_cast<const struct sockaddr*>(&bind_addr), 0);
    if (result != 0) {
        throw std::runtime_error("Failed to bind UDP socket: " + std::string(uv_strerror(result)));
    }

    uv_udp_recv_start(udp_socket_.get(),
        [](uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
            buf->base = static_cast<char*>(malloc(suggested_size));
            buf->len = suggested_size;
        },
        [](uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned flags) {
            auto* tello = static_cast<Tello*>(handle->data);
            if (nread > 0) {
                tello->last_response_ = std::string(buf->base, nread);
                tello->response_received_ = true;
                std::cout << "Received UDP data: " << tello->last_response_ << std::endl;
            } else if (nread < 0) {
                std::cerr << "UDP receive error: " << uv_strerror(nread) << std::endl;
            }
            free(buf->base);
        });
}

std::optional<std::string> Tello::connect() {
    return send_command("command");
}

std::optional<std::string> Tello::send_command(std::string_view cmd) {
    if (!udp_socket_) {
        std::cerr << "UDP socket not initialized" << std::endl;
        return std::nullopt;
    }

    uv_buf_t buf = uv_buf_init(const_cast<char*>(cmd.data()), cmd.size());
    auto* req = new uv_udp_send_t(); // Allocate manually
    req->data = nullptr; // Optional: store data if needed in callback


    struct sockaddr_in tello_addr;
    uv_ip4_addr(ip_.c_str(), port_, &tello_addr);

    int result = uv_udp_send(req, udp_socket_.get(), &buf, 1,
                            reinterpret_cast<const struct sockaddr*>(&tello_addr),
                            [](uv_udp_send_t* req, int status) {
                                if (status) {
                                    std::cerr << "UDP send failed: " << uv_strerror(status) << std::endl;
                                }
                                // Free the request structure here
                                delete req;
                            });
    if (result != 0) {
        std::cerr << "Failed to send command: " << uv_strerror(result) << std::endl;
        return std::nullopt;
    }

    response_received_ = false;
    for (int i = 0; i < 10 && !response_received_; ++i) {
        uv_run(&loop_, UV_RUN_ONCE);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!response_received_) {
        std::cerr << "No response received for command: " << cmd << std::endl;
        return std::nullopt;
    }
    return last_response_;
}