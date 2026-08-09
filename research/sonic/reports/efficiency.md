# Efficiency benchmark

Environment: host=Darwin/arm64 25.5.0, kapre=0.3.6, name=openl3, openl3=0.4.0, profile=musicpack-sonic-v1-d5d3cc100ac4e01d, tensorflow=2.15.1, weight_file=openl3_audio_mel256_music.h5, weight_sha256=624ee7b1dd5ff87e18073f66fd8b2052bebb8ac70210e9c0937c0c940c63e9d6, weights_cc_license=CC BY 4.0.

| metric | n | mean | std | min | median | max |
|---|---|---|---|---|---|---|
| wall s / track | 3 | 3 | 1.223 | 1.086123688475059 | 0.45 | 0.46 | 2.759 |
| wall s / min audio | 3 | 3 | 8.0 | 0.0 | 8.0 | 8.0 | 8.0 |
| realtime factor | 3 | 3 | 0.15283333333333335 | 0.1357419692734057 | 0.0562 | 0.0575 | 0.3448 |


Realtime factor: seconds of analysis per second of audio (0.15x = 1 min of music in 9 s).