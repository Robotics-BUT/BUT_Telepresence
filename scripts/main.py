import datetime
import random
import time
import matplotlib.pyplot as plt
import numpy as np

file_paths = ['A.txt', 'B.txt', 'C.txt']
mock_time_a = 1706013677873000
mock_time_b = 1706013677893000
mock_time_c = 1706013677913000


def generate_mock_timestamp_files(num_timestamps_per_line, num_lines):
    file_num = 0
    for file_path in file_paths:
        offset = mock_time_a if file_num == 0 else mock_time_b if file_num == 1 else mock_time_c
        with open(file_path, 'w') as file:
            for _ in range(num_lines):
                timestamps = [random.randint(10, 5000) for _ in range(num_timestamps_per_line[file_num])]
                timestamps.sort()
                s = [t + offset for t in timestamps]
                timestamps_line = ",".join(map(str, s))
                file.write(timestamps_line + "\n")
                offset = offset + 100000
            file_num = file_num + 1


def read_timestamps(file_path):
    with open(file_path, 'r') as file:
        timestamps = [list(map(int, line.strip().split(','))) for line in file]
    return timestamps


def show_histograms(list_of_lists):
    num_elements = len(list_of_lists[0])

    for i in range(num_elements):
        column_data = [row[i] for row in list_of_lists]

        plt.figure(figsize=(10, 6))
        plt.hist(column_data, bins=20, edgecolor='black')
        plt.title(f'Histogram of Element {i+1}')
        plt.xlabel(f'Element {i+1} Value')
        plt.ylabel('Frequency')
        plt.grid(True)
        plt.show()


def main():
    num_timestamps_per_line = [2, 3, 3]  # Change this list based on your requirements
    num_lines = 10

    generate_mock_timestamp_files(num_timestamps_per_line, num_lines)

    # Read timestamps from each file
    timestamps_a = read_timestamps(file_paths[0])
    timestamps_b = read_timestamps(file_paths[1])
    timestamps_c = read_timestamps(file_paths[2])

    # Ensure all files have the same number of lines
    if len(timestamps_a) != len(timestamps_b) or len(timestamps_a) != len(timestamps_c):
        print("Error: Files do not have the same number of lines.")
        return

    # Match the timestamps and compute latencies
    stamps = zip(timestamps_a, timestamps_b, timestamps_c)
    latencies = []
    for a, b, c in stamps:
        concat = a + b + c
        diff = [b - a for a, b in zip(concat, concat[1:])]
        print(diff)
        latencies.append(diff)

    #print(latencies)
    show_histograms(latencies)

if _name_ == "_main_":
    main()