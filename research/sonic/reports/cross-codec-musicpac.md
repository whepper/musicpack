# Cross-codec stability — profile musicpack-sonic-v1-d5d3cc100ac4e01d

For each source track: cosine(source-embedding, flac-embedding) and cosine(source-embedding, mpc-q6-embedding). Ideal is ~1.0; lossy codecs should not change what the music 'sounds like'.

| metric | n | mean | std | min | median | max |
|---|---|---|---|---|---|---|
| cos_source_flac | 3 | 1.0000 | 0.0000 | 1.0000 | 1.0000 | 1.0000 |
| cos_source_mpc | 3 | 0.9870 | 0.0001 | 0.9869 | 0.9870 | 0.9871 |
| cos_flac_mpc | 3 | 0.9870 | 0.0001 | 0.9869 | 0.9870 | 0.9871 |
