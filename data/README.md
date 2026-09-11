# Benchmark datasets

Per-frame latency telemetry produced by the framework's own instrumentation, for every run
underlying the evaluation and the pilot study in the accompanying paper. Each frame carries
per-stage timings embedded in RTP header extensions and recorded at the headset, so the data
here is what the running system measured about itself, not a post-hoc reconstruction.

## Layout

```
data/
  benchmark/            evaluation runs over a 6 GHz Wi-Fi 6E link
    A-1/                quiet link, steady cell (the headline configuration)
    A-T/                quiet link, reconfiguration timeline
    B-1/                contended link, steady cell
    B-T/                contended link, reconfiguration timeline
  pilot/                operator study runs, two per session
    S01_baseline/       session 01, baseline latency
    S01_delayed/        session 01, +250 ms injected receiver-side delay
    ...
```

Every run directory holds:

| file | contents |
|---|---|
| `telemetry.csv.gz` | per-frame latency telemetry, schema below. Note the deduplication step under *Reading it* |
| `markers.csv` | `UTC timestamp,label` for each reconfiguration or run event |
| `metadata.txt` | run conditions, network topology, the configuration observed in the window, and any operator notes |
| `session_state.json` | pilot runs only: condition, layout, run window, and the pre-start and in-run link profiles |

## Telemetry schema

31 columns. All durations are **microseconds** unless stated otherwise.

**Identity and rate**

| column | meaning |
|---|---|
| `time` | headset wall-clock timestamp, UTC, nanosecond resolution |
| `frame_id` | monotonic frame counter |
| `fps` | delivered frame rate, instantaneous |

**Pipeline stages, in order.** These sum to `total_latency_us` by construction, which is what
makes degradation attributable to a stage instead of only visible in aggregate.

| column | stage |
|---|---|
| `camera_us` | sensor exposure and ISP. **Modelled, not probed**: the image sensor exposes no per-frame capture timestamp, so this is a fixed interval, not a measurement |
| `vidConv_us` | colour-space conversion |
| `enc_us` | hardware encode |
| `rtpPay_us` | RTP payloading |
| `udpStream_us` | network transport, sender to receiver |
| `jbHold_us` | time held in the receiver's pre-decoder queue. The pilot's delayed condition is injected here |
| `rtpDepay_us` | RTP depayloading |
| `dec_us` | hardware decode |
| `appsink_us` | handoff from the decoder to the renderer |
| `presentation_us` | display tail, derived from the runtime's predicted display time |
| `total_latency_us` | sum of the above |

**Link quality**, per eye: `left_lost` / `right_lost` (cumulative lost RTP packets),
`left_jitter_us` / `right_jitter_us`, `left_bitrate_bps` / `right_bitrate_bps`,
`left_rtx` / `right_rtx` (retransmissions).

**Configuration in force**, repeated on every row so any slice is self-describing:
`codec` (`0` JPEG, `1` VP8, `2` VP9, `3` H.264, `4` H.265), `fps_config`, `bitrate_cfg` (bits/s),
`resolution_width`, `resolution_height`, `video_mode` (`0` stereo, `1` mono).

**Clock synchronisation**: `ntp_offset_us` (headset-to-robot offset in force for this frame),
`ntp_synced` (`1` when the estimate is fresh), `time_since_ntp_sync_us`. The per-stage split
depends on both ends agreeing on time, so these columns are what let you judge whether a given
frame's decomposition is trustworthy.

### Reading it

Two filters are required before any statistic will match the paper.

**Drop pre-roll rows.** Rows with `total_latency_us == 0` were recorded before the first frame
had traversed the whole chain.

**Deduplicate on `frame_id`, keeping the first row.** The exporter emits a row per telemetry
update, and a frame can receive more than one, so the raw files contain roughly 1.6 rows per
delivered frame. Counting rows instead of frames inflates `n` and shifts the median by a few
tenths of a millisecond.

```python
import pandas as pd
df = pd.read_csv("data/benchmark/A-1/telemetry.csv.gz")     # gzip handled automatically
df = df[df.total_latency_us > 0]                            # 1. drop pre-roll
df = df.drop_duplicates(subset="frame_id", keep="first")    # 2. one row per frame
print(len(df), (df.total_latency_us / 1000).median())       # 44097  78.56
```

That reproduces the published A-1 figures exactly: n = 44,097, median 78.6 ms,
interquartile range 75.4 to 82.9 ms, 95th percentile 101.1 ms.

## Anonymisation

The pilot runs come from a study with human participants, approved by the research ethics
committee of the Faculty of Electrical Engineering and Communication, Brno University of
Technology (ref. 6b/18360/26). Participants consented to publication of pseudonymised data.

Session identifiers `S01`–`S08` are arbitrary and are **not** in the order participants were
recruited. The two runs within a session belong to the same operator, which is what the
within-subject design requires; nothing links a session to a person, and no mapping to any
internal record is published.

This directory contains **machine telemetry only**. Questionnaire responses, demographics and
free-text debrief answers are deliberately not released: with eight participants a single
demographic cell can be identifying. Participant characteristics and outcome contrasts appear
in the paper in aggregate form only.

Device MAC addresses and operating-system usernames have been redacted from `metadata.txt`.
Private (RFC 1918) addresses are retained, since they are non-routable and the network topology
is needed to interpret which hop each measurement describes.
