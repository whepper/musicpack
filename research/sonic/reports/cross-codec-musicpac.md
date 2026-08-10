# Cross-codec stability — profile musicpack-sonic-v1-99459eca2f647cbe

For each source track: cosine(source-embedding, flac-embedding) and cosine(source-embedding, mpc-q6-embedding). Ideal is ~1.0; lossy codecs should not change what the music 'sounds like'.

| metric | n | mean | std | min | median | max |
|---|---|---|---|---|---|---|
| cos_source_flac | 20 | 0.9999 | 0.0000 | 0.9999 | 0.9999 | 1.0000 |
| cos_source_mpc | 20 | 0.9998 | 0.0001 | 0.9995 | 0.9998 | 0.9999 |
| cos_flac_mpc | 20 | 0.9998 | 0.0001 | 0.9995 | 0.9999 | 1.0000 |
