## 🛠 TROUBLESHOOTING GUIDE: DJI Tello PoC with RabbitMQ

This guide helps diagnose issues where `tello_controller` is not receiving messages from `flight_controller` via RabbitMQ in the DJI Tello PoC system.

---

### ✅ Test the PoC

#### 1. Connect to Tello Wi-Fi

* Join network: `TELLO-XXXXXX`
* Verify:

```bash
ping 192.168.10.1
```

#### 2. Start RabbitMQ

```bash
sudo apt install rabbitmq-server
sudo systemctl start rabbitmq-server
```

#### 3. Test Environment

* Use a \~3m x 3m clear indoor space
* Expected flight sequence:
  `takeoff → forward 50 → cw 90 (x4) → land`

---

### 📝 System Overview

* **OS:** Ubuntu 22.04
* **Lang:** C++17
* **Transport:** libuv (UDP)
* **Messaging:** RabbitMQ via AMQP-CPP
* **Dependencies (via Conan):**

  * amqp-cpp/4.3.26
  * libuv/1.48.0
  * zlib/1.3.1
  * openssl/3.3.2

---

### 🐇 Step 1: Enable and Access RabbitMQ Logs

#### Find Logs:

```bash
/var/log/rabbitmq/rabbit@<hostname>.log
```

#### Increase Log Verbosity:

Edit or create `/etc/rabbitmq/rabbitmq.conf`:

```
log.console.level = debug
```

Restart:

```bash
sudo systemctl restart rabbitmq-server
```

View logs:

```bash
tail -f /var/log/rabbitmq/rabbit@<hostname>.log
journalctl -u rabbitmq-server.service -n 100
```

---

### 🔎 Step 2: Use RabbitMQ Management UI

#### Enable Management Plugin:

```bash
sudo rabbitmq-plugins enable rabbitmq_management
```

Access: [http://localhost:15672](http://localhost:15672) (`guest` / `guest`)

#### Check Queue `tello_commands`:

* Should exist and be durable
* If messages accumulate → consumer not working
* If empty → publisher not working

#### Bindings Tab:

* Should show binding to default exchange (`""`) with routing key `tello_commands`

#### Consumers Tab:

* `tello_controller` must appear as an active consumer

---

### 🧾 Step 3: Analyze Logs for Errors

#### Connection Errors:

Look for:

```
{socket_error,econnrefused}
```

Fix:

```bash
sudo systemctl status rabbitmq-server
sudo netstat -tuln | grep 5672
sudo ufw status
```

#### Authentication Errors:

Look for:

```
HTTP access denied
can't access vhost
```

Check permissions:

```bash
sudo rabbitmqctl list_permissions -p /
```

Expect:

```
guest   .*   .*   .*
```

Set if missing:

```bash
sudo rabbitmqctl set_permissions -p / guest ".*" ".*" ".*"
```

---

### 🔄 Step 4: Additional Checks

#### Network Test:

```bash
telnet localhost 5672
```

#### Queue Stats:

```bash
sudo rabbitmqctl list_queues name messages consumers
```

#### AMQP Connection Logs:

Check for:

```
accepting AMQP connection <...>
```

If absent → client not connecting.

---

### ✅ Summary Checklist

* [ ] Tello Wi-Fi connection works
* [ ] RabbitMQ running & accessible
* [ ] Management UI enabled
* [ ] Queue `tello_commands` is bound
* [ ] Consumer (`tello_controller`) connected
* [ ] No auth/connection errors in logs

---

For persistent issues, inspect logs and retry minimal end-to-end tests before escalating.
