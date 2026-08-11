# Cross-codec stability — profile musicpack-sonic-v1-5e6059049aab4cee

For each source track: cosine(source-embedding, flac-embedding) and cosine(source-embedding, mpc-q6-embedding). Ideal is ~1.0; lossy codecs should not change what the music 'sounds like'.

| metric | n | mean | std | min | median | max |
|---|---|---|---|---|---|---|
| cos_source_flac | 10 | 1.0000 | 0.0000 | 1.0000 | 1.0000 | 1.0000 |
| cos_source_mpc | 10 | 1.0000 | 0.0000 | 1.0000 | 1.0000 | 1.0000 |
| cos_flac_mpc | 10 | 1.0000 | 0.0000 | 1.0000 | 1.0000 | 1.0000 |
