"""Decoder output delay (frames held inside libavcodec) for console-like streams.
Mirrors RemotePlaySource: one access unit per send, thread_count=1, drain after each send.
Compares default flags (PushFPS today) with AV_CODEC_FLAG_LOW_DELAY, with and without a lost frame."""
import av, numpy as np, sys
from fractions import Fraction
from av.codec.context import Flags
def encode(codec, n=180, w=640, h=360):
    enc = av.CodecContext.create(codec, 'w')
    enc.width, enc.height, enc.pix_fmt = w, h, 'yuv420p'
    enc.time_base = Fraction(1001, 60000); enc.framerate = Fraction(60000, 1001)
    enc.gop_size = 600
    enc.options = {'tune': 'zerolatency', 'preset': 'ultrafast', 'bframes': '0'} if codec=='libx264' else \
                  {'tune': 'zerolatency', 'preset': 'ultrafast', 'x265-params': 'bframes=0:repeat-headers=1:log-level=0'}
    out=[]
    for i in range(n):
        img = np.zeros((h, w, 3), np.uint8); img[:, (i*4) % w:(i*4) % w + 40] = 255
        f = av.VideoFrame.from_ndarray(img, 'rgb24').reformat(format='yuv420p'); f.pts = i
        out += [bytes(p) for p in enc.encode(f)]
    out += [bytes(p) for p in enc.encode(None)]
    return out
def run(dec_name, aus, low_delay, drop=None):
    dec = av.CodecContext.create(dec_name, 'r'); dec.thread_count = 1
    if low_delay: dec.flags |= Flags.low_delay
    sent = got = refused = 0; worst = 0; lag_at_end = 0
    for i, au in enumerate(aus):
        if drop is not None and i == drop: continue          # a lost P frame (POC gap)
        try: frames = dec.decode(av.Packet(au))
        except Exception:
            # A missing reference: the decoder refuses the picture. Not a delay; PushFPS's
            # ingress already skips to the next keyframe after a gap, so these never reach it.
            refused += 1; continue
        sent += 1; got += len(frames); worst = max(worst, sent - got); lag_at_end = sent - got
    # Frames the decoder was really holding come out when it is flushed; pictures it
    # discarded for a missing reference (after the lost frame, until a keyframe) do not.
    try: held = len(dec.decode(None))
    except Exception: held = 0
    return sent, got, held, lag_at_end - held, refused
for enc, dec in (('libx264', 'h264'), ('libx265', 'hevc')):
    aus = encode(enc)
    for low in (False, True):
        for drop in (None, 60):
            s, g, held, discarded, refused = run(dec, aus, low, drop)
            print(f"{dec:5s} low_delay={int(low)} lostFrame={'yes' if drop else 'no ':3s} sent={s} decodedImmediately={g} heldInDecoder={held} discardedAfterLoss={discarded + refused}")
