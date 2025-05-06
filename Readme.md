# Tello Drone PoC (Proof of Concept)

A minimal toy project to control a DJI Tello drone using RabbitMQ and modern C++ on Ubuntu 22.04.

## Components

- `flight_controller`: Publishes flight commands to RabbitMQ
- `tello_controller`: Subscribes to flight commands and sends them to the drone via UDP

## Dependencies

- [AMQP-CPP](https://github.com/CopernicaMarketingSoftware/AMQP-CPP) (v4.3.26)
- [libuv](https://github.com/libuv/libuv) (v1.48.0)
- [OpenSSL](https://www.openssl.org/) (v3.3.2)
- [zlib](https://zlib.net/) (v1.3.1)
- [Conan](https://conan.io/) for dependency management

## Setup

1. **Connect to Tello Wi-Fi**

   Join `TELLO-XXXXXX` and verify connectivity:
   ```bash
   ping 192.168.10.1

2. **Start RabbitMQ**

   ```bash
   sudo apt install rabbitmq-server
   sudo systemctl start rabbitmq-server
   ```

3. **Build the System**

   Use `conan` and `cmake` to install dependencies and build:

   ```bash
   conan install . --build=missing
   cmake -B build
   cmake --build build
   ```

4. **Test Environment**

   Prepare a clear indoor space (\~3m x 3m) for the square flight pattern.

## Autonomous Flight Pattern

* Takeoff
* Forward 50 cm
* Clockwise 90° (repeated 4 times)
* Land

## Troubleshooting

If `tello_controller` is not receiving messages, see the [RabbitMQ Troubleshooting Guide](docs/troubleshooting.md) for step-by-step diagnostics on queues, consumers, log verbosity, permissions, and more.

---

