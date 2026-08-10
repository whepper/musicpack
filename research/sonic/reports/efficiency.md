# Efficiency benchmark

Environment: host=Darwin/arm64 25.5.0, kapre=0.3.6, name=openl3, openl3=0.4.0, profile=musicpack-sonic-v1-99459eca2f647cbe, tensorflow=2.15.1, weight_file=openl3_audio_mel256_music.h5, weight_sha256=624ee7b1dd5ff87e18073f66fd8b2052bebb8ac70210e9c0937c0c940c63e9d6, weights_cc_license=CC BY 4.0.

| metric | n | mean | std | min | median | max |
|---|---|---|---|---|---|---|
| wall s / track | 10 | 15.3649 | 4.2652 | 8.4830 | 15.0595 | 25.4580 |
| wall s / min audio | 10 | 3.3048 | 0.1505 | 3.1095 | 3.2586 | 3.5805 |
| realtime factor | 10 | 0.0551 | 0.0025 | 0.0518 | 0.0543 | 0.0597 |


Realtime factor: seconds of analysis per second of audio (0.15x = 1 min of music in 9 s). Wall s / min audio is the same quantity scaled to one minute of music.