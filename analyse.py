import re
import sys

filename = sys.argv[1]   # you'll run: python3 analyze.py results_K3.txt

array_sizes = []
throughputs = []

with open(filename) as f:
    for line in f:
        # look for lines like: burst 0 array_size=2048 grows=7 shrinks=0
        m = re.search(r"array_size=(\d+)", line)
        if m:
            array_sizes.append(int(m.group(1)))

        # look for lines like: Owner loop took 9802 us (0.9802 us/task)
        m2 = re.search(r"\(([\d.]+) us/task\)", line)
        if m2:
            throughputs.append(float(m2.group(1)))

avg_array_size = sum(array_sizes) / len(array_sizes)
avg_throughput = sum(throughputs) / len(throughputs)

print(f"Number of burst lines found: {len(array_sizes)}")
print(f"Number of runs found: {len(throughputs)}")
print(f"Average array_size: {avg_array_size:.1f}")
print(f"Average us/task: {avg_throughput:.4f}")