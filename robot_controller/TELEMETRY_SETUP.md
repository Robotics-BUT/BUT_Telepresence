# Telemetry Setup Guide - InfluxDB + Grafana

Quick guide to set up real-time telemetry monitoring for your telepresence robot.

## Quick Install

### 1. Install InfluxDB

```bash
curl -O https://www.influxdata.com/d/install_influxdb3.sh && sh install_influxdb3.sh
source ~/.bashrc
influxdb3 create token --admin # Keep the output somewhere safe

# To actually run the server (without authentification!)
influxdb3 serve --node-id but_telepresence_telemetry --object-store file --without-auth
```

### 2. Install Grafana

```bash
# Ubuntu/Debian
sudo apt-get install -y apt-transport-https wget
sudo mkdir -p /etc/apt/keyrings/
wget -q -O - https://apt.grafana.com/gpg.key | gpg --dearmor | sudo tee /etc/apt/keyrings/grafana.gpg > /dev/null
echo "deb [signed-by=/etc/apt/keyrings/grafana.gpg] https://apt.grafana.com stable main" | sudo tee -a /etc/apt/sources.list.d/grafana.list
sudo apt-get update
sudo apt-get install grafana

sudo systemctl start grafana-server.service
sudo systemctl enable grafana-server.service

```

### 3. Install Python Client

```bash
pip install influxdb3-python
```

### 4. Enable Telemetry

Edit `robot_controller/config.yaml`:

```yaml
telemetry:
  enabled: true  # Change from false to true
```

### 5. Configure Grafana

1. Open browser: **http://localhost:3000** (login: admin/admin)
2. Add InfluxDB data source:
   - Settings → Data Sources → Add data source → InfluxDB
   - Name: `Robot Telemetry`
   - Query Language: `InfluxQL`
   - URL: `http://localhost:8086`
   - Database: `robot_telemetry`
   - Click **Save & Test**

3. Import dashboard:
   - Dashboards → Import → Upload JSON file
   - Select `grafana_dashboard.json`
   - Choose data source: `Robot Telemetry`
   - Click **Import**

## Dashboard Features

Your dashboard includes:

- **FPS Gauge** - Real-time frames per second
- **Total Pipeline Latency** - End-to-end latency graph
- **Pipeline Stages Breakdown** - Stacked area chart showing each stage
- **Frame Rate Over Time** - Historical FPS tracking
- **NTP Sync Status** - NTP synchronization indicator
- **Current Frame ID** - Latest processed frame
- **NTP Offset** - Clock synchronization offset
- **Latency Percentiles** - p50, p90, p99 statistics

Auto-refreshes every 5 seconds.

## Troubleshooting

### Check if services are running:
```bash
sudo systemctl status influxdb
sudo systemctl status grafana-server
```

### Verify data in InfluxDB:
```bash
influx
> SHOW DATABASES
> USE robot_telemetry
> SELECT * FROM pipeline_metrics LIMIT 10
```

### Check logs:
```bash
# InfluxDB
sudo journalctl -u influxdb -f

# Grafana
sudo journalctl -u grafana-server -f
```

## Remote Access

To access from another device:

```bash
# Find your IP
hostname -I

# Access Grafana at:
# http://<your-ip>:3000
```

## Collected Metrics

- `fps` - Frames per second
- `vidConv_us` - Video conversion time
- `enc_us` - Encoding time
- `rtpPay_us` - RTP payload time
- `udpStream_us` - UDP streaming time
- `rtpDepay_us` - RTP depayload time
- `dec_us` - Decoding time
- `presentation_us` - Presentation time
- `total_latency_us` - Total pipeline latency
- `ntp_offset_us` - NTP clock offset
- `ntp_synced` - NTP sync status (0/1)
- `time_since_ntp_sync_us` - Time since last NTP sync
- `frame_id` - Frame identifier

## Cost

**$0** - Everything is free and open-source!

---

That's it! Enjoy your real-time telemetry dashboard 🚀
