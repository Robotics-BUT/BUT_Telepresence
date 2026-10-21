#!/usr/bin/env python3
"""
Export telemetry data from InfluxDB to CSV.

Usage:
    python export_telemetry.py                     # Export all data
    python export_telemetry.py -o data.csv         # Custom output file
    python export_telemetry.py --last "1 hour"     # Export last hour
    python export_telemetry.py --last "30 minutes" # Export last 30 minutes
    python export_telemetry.py --host http://192.168.1.100:8181  # Remote host
"""

import argparse
import sys
from pathlib import Path
from datetime import datetime

try:
    from influxdb_client_3 import InfluxDBClient3
except ImportError:
    print("Error: influxdb3-python package not installed.")
    print("Install with: pip install influxdb3-python")
    sys.exit(1)


def export_to_csv(
    host: str,
    database: str,
    output_path: Path,
    time_filter: str = None,
    start: str = None,
    end: str = None
):
    """Export telemetry data to CSV."""
    print(f"Connecting to InfluxDB at {host}...")

    client = InfluxDBClient3(
        host=host,
        database=database,
    )

    # Full chain, in pipeline order, plus the per-eye transport counters and the
    # config columns that identify which cell a row belongs to.
    # Mixed-case names need double quotes in SQL or DataFusion lowercases them
    # and silently returns NULL.
    columns = [
        "time",
        "frame_id",
        "fps",
        # --- latency chain, in order ---
        "camera_us",
        '"vidConv_us"',
        "enc_us",
        '"rtpPay_us"',
        '"udpStream_us"',
        '"jbHold_us"',
        '"rtpDepay_us"',
        "dec_us",
        "appsink_us",
        "presentation_us",
        "total_latency_us",
        # --- per-eye transport health: loss is what distinguishes a real
        # latency excursion from a packet-loss recovery stall ---
        "left_lost",
        "right_lost",
        "left_jitter_us",
        "right_jitter_us",
        "left_bitrate_bps",
        "right_bitrate_bps",
        "left_rtx",
        "right_rtx",
        # --- config provenance: which cell this row belongs to ---
        "codec",
        "fps_config",
        "bitrate_cfg",
        "resolution_width",
        "resolution_height",
        "video_mode",
        # --- clock trust ---
        "ntp_offset_us",
        "ntp_synced",
        "time_since_ntp_sync_us",
    ]

    select_clause = ", ".join(columns)

    where = []
    if time_filter:
        where.append(f"time > now() - INTERVAL '{time_filter}'")
    if start:
        where.append(f"time >= '{start}'")
    if end:
        where.append(f"time <= '{end}'")

    where_clause = (" WHERE " + " AND ".join(where)) if where else ""
    query = f"SELECT {select_clause} FROM pipeline_metrics{where_clause} ORDER BY time"

    print(f"Executing query...")

    try:
        table = client.query(query)
        df = table.to_pandas()
    except Exception as e:
        print(f"Error querying database: {e}")
        sys.exit(1)

    if df.empty:
        print("No data found in database.")
        sys.exit(0)

    # Convert timestamp to readable format
    if 'time' in df.columns:
        df['time'] = df['time'].astype(str)

    # Export to CSV
    df.to_csv(output_path, index=False)

    print(f"Exported {len(df)} rows to {output_path}")
    print(f"Columns: {', '.join(df.columns)}")

    # Print summary statistics
    if 'total_latency_us' in df.columns:
        latency_ms = df['total_latency_us'] / 1000
        print(f"\nLatency summary:")
        print(f"  Mean:   {latency_ms.mean():.2f} ms")
        print(f"  Median: {latency_ms.median():.2f} ms")
        print(f"  Min:    {latency_ms.min():.2f} ms")
        print(f"  Max:    {latency_ms.max():.2f} ms")

    if 'fps' in df.columns:
        print(f"\nFPS summary:")
        print(f"  Mean:   {df['fps'].mean():.2f}")
        print(f"  Min:    {df['fps'].min():.2f}")
        print(f"  Max:    {df['fps'].max():.2f}")

    # Run sanity check. The relay re-logs the last row while waiting for a new
    # frame, so raw row count is NOT frame count -- dedupe by frame_id before
    # computing any statistic. A frozen frame_id means the headset stopped
    # delivering (app suspended, e.g. taken off the head), not a slow pipeline.
    if 'frame_id' in df.columns:
        uniq = df['frame_id'].nunique()
        span = int(df['frame_id'].max() - df['frame_id'].min())
        print(f"\nRun sanity:")
        print(f"  Rows:              {len(df)}")
        print(f"  Distinct frame_id: {uniq}  ({len(df)/max(uniq,1):.1f}x re-logged per frame)")
        print(f"  frame_id span:     {span}")
        if span:
            print(f"  Logged coverage:   {100*uniq/span:.1f}% of delivered frames")
        if uniq <= 1:
            print("  !! frame_id is FROZEN -- headset stopped delivering. Discard this run.")

    # Packet loss drives 'dec_us' excursions; report it so a contaminated run is
    # obvious at capture time rather than during analysis.
    for eye in ("left", "right"):
        col = f"{eye}_lost"
        if col in df.columns and len(df[col].dropna()):
            lost = int(df[col].max() - df[col].min())
            print(f"  {eye+' RTP lost:':<18} {lost}")


def main():
    parser = argparse.ArgumentParser(
        description="Export telemetry data from InfluxDB to CSV"
    )
    parser.add_argument(
        "-o", "--output",
        type=Path,
        default=None,
        help="Output CSV file path (default: telemetry_YYYYMMDD_HHMMSS.csv)"
    )
    parser.add_argument(
        "--last",
        type=str,
        default=None,
        help="Export only last N time (e.g., '1 hour', '30 minutes', '1 day')"
    )
    parser.add_argument(
        "--start",
        type=str,
        default=None,
        help="Window start, ISO 8601 UTC (e.g. '2026-07-31T09:29:00Z'). "
             "Prefer this over --last when several runs happened in one session. "
             "NOTE: the robot clock is UTC, the PC is local."
    )
    parser.add_argument(
        "--end",
        type=str,
        default=None,
        help="Window end, ISO 8601 UTC (e.g. '2026-07-31T09:32:00Z')"
    )
    parser.add_argument(
        "--host",
        type=str,
        default="http://localhost:8181",
        help="InfluxDB host (default: http://localhost:8181)"
    )
    parser.add_argument(
        "--database",
        type=str,
        default="but_telepresence_telemetry",
        help="InfluxDB database (default: but_telepresence_telemetry)"
    )

    args = parser.parse_args()

    # Generate default output filename with timestamp
    if args.output is None:
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        args.output = Path(f"telemetry_{timestamp}.csv")

    export_to_csv(
        host=args.host,
        database=args.database,
        output_path=args.output,
        time_filter=args.last,
        start=args.start,
        end=args.end
    )


if __name__ == "__main__":
    main()
