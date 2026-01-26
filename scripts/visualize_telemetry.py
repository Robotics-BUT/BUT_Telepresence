#!/usr/bin/env python3
"""
Visualize telemetry data from exported CSV.

Usage:
    python visualize_telemetry.py telemetry_20260126_120000.csv
    python visualize_telemetry.py telemetry.csv -o report.png  # Save to file
    python visualize_telemetry.py telemetry.csv --latency-max 100 --fps-min 25 --fps-max 35
    python visualize_telemetry.py telemetry.csv --time-start 2026-01-26T12:00:00 --time-end 2026-01-26T12:05:00
"""

import argparse
import sys
from pathlib import Path

import pandas as pd
import matplotlib.pyplot as plt


def load_data(csv_path: Path) -> pd.DataFrame:
    """Load and preprocess telemetry CSV."""
    df = pd.read_csv(csv_path)
    df['time'] = pd.to_datetime(df['time'])

    # Convert microseconds to milliseconds for readability
    latency_cols = [c for c in df.columns if c.endswith('_us')]
    for col in latency_cols:
        ms_col = col.replace('_us', '_ms')
        df[ms_col] = df[col] / 1000

    return df


def plot_latency_breakdown(df: pd.DataFrame, ax: plt.Axes, latency_max: float = None):
    """Stacked area chart of latency stages."""
    stages = ['camera_ms', 'vidConv_ms', 'enc_ms', 'rtpPay_ms',
              'udpStream_ms', 'rtpDepay_ms', 'dec_ms', 'presentation_ms']

    # Filter to existing columns
    stages = [s for s in stages if s in df.columns]

    ax.stackplot(df['time'], [df[s] for s in stages],
                 labels=[s.replace('_ms', '') for s in stages],
                 alpha=0.8)
    ax.set_ylabel('Latency (ms)')
    ax.set_title('Pipeline Latency Breakdown')
    ax.legend(loc='upper left', fontsize='small')
    ax.grid(True, alpha=0.3)
    if latency_max is not None:
        ax.set_ylim(0, latency_max)


def plot_total_latency(df: pd.DataFrame, ax: plt.Axes, latency_max: float = None):
    """Line plot of total latency with rolling average."""
    ax.plot(df['time'], df['total_latency_ms'], alpha=0.3, label='Raw')

    # Rolling average (50 samples)
    window = min(50, len(df) // 10) if len(df) > 10 else 1
    rolling = df['total_latency_ms'].rolling(window=window, center=True).mean()
    ax.plot(df['time'], rolling, linewidth=2, label=f'Rolling avg ({window})')

    ax.set_ylabel('Latency (ms)')
    ax.set_title('Total End-to-End Latency')
    ax.legend()
    ax.grid(True, alpha=0.3)
    if latency_max is not None:
        ax.set_ylim(0, latency_max)


def plot_fps(df: pd.DataFrame, ax: plt.Axes, fps_min: float = None, fps_max: float = None):
    """Line plot of FPS over time."""
    ax.plot(df['time'], df['fps'], alpha=0.5)
    ax.axhline(y=df['fps'].mean(), color='r', linestyle='--',
               label=f'Mean: {df["fps"].mean():.1f}')
    ax.set_ylabel('FPS')
    ax.set_xlabel('Time')
    ax.set_title('Frame Rate')
    ax.legend()
    ax.grid(True, alpha=0.3)
    if fps_min is not None or fps_max is not None:
        ax.set_ylim(fps_min, fps_max)


def plot_latency_histogram(df: pd.DataFrame, ax: plt.Axes, latency_max: float = None):
    """Histogram of total latency."""
    data = df['total_latency_ms']
    if latency_max is not None:
        data = data[data <= latency_max]
    ax.hist(data, bins=50, edgecolor='black', alpha=0.7)
    ax.axvline(df['total_latency_ms'].median(), color='r', linestyle='--',
               label=f'Median: {df["total_latency_ms"].median():.1f} ms')
    ax.axvline(df['total_latency_ms'].quantile(0.95), color='orange', linestyle='--',
               label=f'95th: {df["total_latency_ms"].quantile(0.95):.1f} ms')
    ax.set_xlabel('Latency (ms)')
    ax.set_ylabel('Count')
    ax.set_title('Latency Distribution')
    ax.legend()
    if latency_max is not None:
        ax.set_xlim(0, latency_max)


def main():
    parser = argparse.ArgumentParser(description="Visualize telemetry CSV data")
    parser.add_argument("csv_file", type=Path, help="Path to telemetry CSV file")
    parser.add_argument("-o", "--output", type=Path, help="Save plot to file")

    # Time range options
    parser.add_argument("--time-start", type=str,
                        help="Start time for filtering (ISO format, e.g., 2026-01-26T12:00:00)")
    parser.add_argument("--time-end", type=str,
                        help="End time for filtering (ISO format, e.g., 2026-01-26T12:05:00)")

    # Axis scale options
    parser.add_argument("--latency-max", type=float,
                        help="Maximum value for latency Y-axis (ms)")
    parser.add_argument("--fps-min", type=float,
                        help="Minimum value for FPS Y-axis")
    parser.add_argument("--fps-max", type=float,
                        help="Maximum value for FPS Y-axis")

    args = parser.parse_args()

    if not args.csv_file.exists():
        print(f"Error: File not found: {args.csv_file}")
        sys.exit(1)

    df = load_data(args.csv_file)
    print(f"Loaded {len(df)} samples from {df['time'].min()} to {df['time'].max()}")

    # Apply time range filtering
    if args.time_start:
        time_start = pd.to_datetime(args.time_start)
        df = df[df['time'] >= time_start]
        print(f"Filtered to start at {time_start}")
    if args.time_end:
        time_end = pd.to_datetime(args.time_end)
        df = df[df['time'] <= time_end]
        print(f"Filtered to end at {time_end}")

    if len(df) == 0:
        print("Error: No data remaining after time filtering")
        sys.exit(1)

    print(f"Rendering {len(df)} samples")

    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    fig.suptitle(f'Telemetry Analysis: {args.csv_file.name}', fontsize=14)

    plot_latency_breakdown(df, axes[0, 0], latency_max=args.latency_max)
    plot_total_latency(df, axes[0, 1], latency_max=args.latency_max)
    plot_fps(df, axes[1, 0], fps_min=args.fps_min, fps_max=args.fps_max)
    plot_latency_histogram(df, axes[1, 1], latency_max=args.latency_max)

    plt.tight_layout()

    if args.output:
        plt.savefig(args.output, dpi=150)
        print(f"Saved to {args.output}")
    else:
        plt.show()


if __name__ == "__main__":
    main()
