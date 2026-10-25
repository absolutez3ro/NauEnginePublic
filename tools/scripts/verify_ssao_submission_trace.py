"""Check actual SSAO scheduling logs: one outstanding job and immediate late-node refill."""

import argparse
import json
import re
from pathlib import Path


def verify(path, require_refill=False):
    log = path.read_text(encoding="utf-8-sig")
    events = re.finditer(
        r"SSAO: (?:submitted frame=(\d+), stage=(submit|receive)|"
        r"received frame=(\d+), source=(\d+), discarded=(true|false)|"
        r"reset pendingSource=(\d+))", log)
    pending = 0
    submitted_frames = set()
    submitted = received = retired = refills = eligible = same_frame = 0
    previous_receive_frame = None
    last_frame = 0
    for event in events:
        submit_frame, stage, receive_frame, source, discarded, reset = event.groups()
        if reset is not None:
            assert int(reset) == pending != 0, "Resize retired an unknown job"
            pending = 0
            retired += 1
            previous_receive_frame = None
            continue
        frame = int(submit_frame if submit_frame is not None else receive_frame)
        assert frame >= last_frame, "Events moved backwards in frame order"
        last_frame = frame
        if submit_frame is not None:
            assert pending == 0, "A new job was submitted before consuming the outstanding result"
            assert frame not in submitted_frames, "The same source frame was submitted twice"
            if stage == "receive":
                assert previous_receive_frame == frame, "Late submission did not follow a receive in this frame"
                refills += 1
            submitted_frames.add(frame)
            pending = frame
            submitted += 1
            previous_receive_frame = None
        else:
            assert int(source) == pending != 0, "Receive is not paired with its submitted source frame"
            assert frame >= pending, "A future job was received"
            eligible += frame > pending
            same_frame += frame == pending
            received += 1
            pending = 0
            previous_receive_frame = frame
    assert submitted and received, "No completed traced SSAO jobs"
    if require_refill:
        assert refills, "No job was submitted immediately from the receive node"
    counters = re.search(r"SSAO: shutdown, .*submitted=(\d+), maxAge=\d+, resubmitted=(\d+)", log)
    assert counters, "No final SSAO scheduling counters"
    assert int(counters[1]) == submitted, "Submission trace disagrees with the shutdown count"
    assert int(counters[2]) == refills, "Late-refill trace disagrees with the shutdown count"
    assert submitted == received + retired + bool(pending), "Outstanding-job accounting mismatch"
    result = dict(submitted=submitted, received=received, retired_on_resize=retired,
                  immediate_refills=refills, receives_from_older_frames=eligible,
                  same_frame_receives=same_frame, pending_at_shutdown=bool(pending))
    print(json.dumps(result))
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--require-refill", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = verify(args.log, args.require_refill)
    if args.output:
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
