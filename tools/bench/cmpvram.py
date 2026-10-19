"""Compare two walks' logged frames: cmpvram.py NAME REF (files in build/run)."""
import sys
a, b = sys.argv[1], sys.argv[2]
def batches(name):
    for line in open(f"build/run/{name}.log", errors="replace"):
        if "frame logged" in line:
            return line.strip()
    return "no frame logged"
print(a, batches(a))
print(b, batches(b))
va = open(f"build/run/{a}.txt.vram", "rb").read()
vb = open(f"build/run/{b}.txt.vram", "rb").read()
print("vram sizes", len(va), len(vb))
if len(va) == len(vb):
    diff = sum(1 for x, y in zip(va, vb) if x != y)
    # The display buffer: fbp 0, fbw 8 (512 px), 448 rows of CT32 = 917504 bytes.
    fb = sum(1 for x, y in zip(va[:917504], vb[:917504]) if x != y)
    print(f"bytes differing: whole VRAM {diff}, display buffer {fb}")
