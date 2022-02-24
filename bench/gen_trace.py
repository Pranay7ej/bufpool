#!/usr/bin/env python3
"""Generate synthetic allocation traces that look like a video player's heap.

Trace format, one op per line:
    a <id> <size>    allocate <size> bytes and remember it as <id>
    f <id>           free <id>

Three shapes:
  steady   - chunks/packets/frames at a fixed quality, buffer hovers near full
  seeky    - like steady, but every ~20 s of content the user seeks: everything
             buffered is dropped and refilled (big free bursts)
  abr      - quality switches up and down, so the dominant sizes keep moving
"""
import argparse
import random

QUALITIES = {  # (frame bytes for NV12 at that height, avg packet bytes)
    360: (640 * 360 * 3 // 2, 6_000),
    480: (854 * 480 * 3 // 2, 10_000),
    720: (1280 * 720 * 3 // 2, 22_000),
}


class Trace:
    def __init__(self):
        self.lines = []
        self.next_id = 0
        self.live = {}

    def alloc(self, size, tag):
        i = self.next_id
        self.next_id += 1
        self.lines.append(f"a {i} {size}")
        self.live[i] = tag
        return i

    def free(self, i):
        self.lines.append(f"f {i}")
        del self.live[i]


def simulate(shape, seconds, seed):
    rng = random.Random(seed)
    t = Trace()
    fps = 30
    height = 480
    net_chunks, packets, frames = [], [], []
    buffered_packets_max = 30 * fps  # ~30 s of demuxed packets
    frame_pool_max = 8
    for sec in range(seconds):
        if shape == "abr" and sec % 15 == 0:
            height = rng.choice(list(QUALITIES))
        frame_bytes, pkt_avg = QUALITIES[height]

        if shape == "seeky" and sec > 0 and sec % 20 == 0:
            for i in packets + frames + net_chunks:
                t.free(i)
            packets, frames, net_chunks = [], [], []

        # Download: a burst of 16-64 KiB socket reads, reassembled into packets.
        refill = max(0, buffered_packets_max - len(packets))
        target = min(refill, fps * rng.randint(1, 3))
        for _ in range(target):
            c = t.alloc(rng.choice([16384, 32768, 65536]), "net")
            net_chunks.append(c)
            # keyframes are much bigger than deltas
            size = int(pkt_avg * (8 if rng.random() < 1 / 60 else rng.uniform(0.4, 1.4)))
            packets.append(t.alloc(size, "pkt"))
            if len(net_chunks) > 4:
                t.free(net_chunks.pop(0))

        # Playback: consume one second of packets, decode into a small frame pool.
        for _ in range(fps):
            if not packets:
                break
            t.free(packets.pop(0))
            frames.append(t.alloc(frame_bytes, "frame"))
            if len(frames) > frame_pool_max:
                t.free(frames.pop(0))
            if rng.random() < 0.1:  # small metadata allocations (subtitles, events)
                t.free(t.alloc(rng.randint(32, 2048), "meta"))

    for i in list(t.live):
        t.free(i)
    return t.lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("shape", choices=["steady", "seeky", "abr"])
    ap.add_argument("--seconds", type=int, default=600)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("-o", "--out", required=True)
    args = ap.parse_args()
    lines = simulate(args.shape, args.seconds, args.seed)
    with open(args.out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{args.out}: {len(lines)} ops")


if __name__ == "__main__":
    main()
