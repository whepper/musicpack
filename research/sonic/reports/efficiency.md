# Efficiency benchmark

Environment: checkpoint_commit=a0b4534a14f58e20944452dff00a22a06ce629d1, host=Darwin/arm64 25.5.0, model=laion/larger_clap_music, name=clap-music, profile=musicpack-sonic-v1-5e6059049aab4cee, projection_dim=512, sample_rate=48000, torch=2.13.0, transformers=5.15.0, weight_file=pytorch_model.bin, weight_sha256=5c289311f4a030d768af7ffbfdecd01b008aa64824211899a4e59f4f9d154fd1, weights_license=apache-2.0.

| metric | n | mean | std | min | median | max |
|---|---|---|---|---|---|---|
| wall s / track | 10 | 6.3316 | 2.0806 | 3.6530 | 6.0465 | 11.8280 |
| wall s / min audio | 10 | 1.3534 | 0.1056 | 1.2972 | 1.3152 | 1.6635 |
| realtime factor | 10 | 0.0226 | 0.0018 | 0.0216 | 0.0219 | 0.0277 |


Realtime factor: seconds of analysis per second of audio (0.15x = 1 min of music in 9 s). Wall s / min audio is the same quantity scaled to one minute of music.