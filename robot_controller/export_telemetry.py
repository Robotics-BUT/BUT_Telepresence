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
    time_filter: str = None
):
    """Export telemetry data to CSV."""
    print(f"Connecting to InfluxDB at {host}...")

    client = InfluxDBClient3(
        host=host,
        database=database,
    )

    # Select specific columns: time, all latency stages, and fps
    # Mixed-case names need double quotes in SQL
    columns = [
        "time",
        "frame_id",
        "fps",
        "camera_us",
        '"vidConv_us"',
        "enc_us",
        '"rtpPay_us"',
        '"udpStream_us"',
        '"rtpDepay_us"',
        "dec_us",
        "presentation_us",
        "total_latency_us",
    ]

    select_clause = ", ".join(columns)

    if time_filter:
        query = f"SELECT {select_clause} FROM pipeline_metrics WHERE time > now() - INTERVAL '{time_filter}' ORDER BY time"
    else:
        query = f"SELECT {select_clause} FROM pipeline_metrics ORDER BY time"

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
        time_filter=args.last
    )


if __name__ == "__main__":
    main()
