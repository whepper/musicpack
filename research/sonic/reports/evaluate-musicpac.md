# Quantitative evaluation — profile musicpack-sonic-v1-120d2ff3377cb5eb

Dataset: 200 tracks, 70 albums, 49 artists (aggregate; see raw). Diagnostics, not ground truth — they never define 'similar'.

| pooling/hop/silence | same_album@10 | same_artist@10 | genre_purity@10 | album_coherence@10 |
|---|---|---|---|---|
| discogs-effnet-multi hop1 mean-norm nosil | 0.1830 | 0.2520 | 0.5225 | 0.0485 |
| discogs-effnet-multi hop1 mean-norm rel--20 | 0.1840 | 0.2525 | 0.5240 | 0.0485 |
| discogs-effnet-release hop1 mean-norm nosil | 0.1830 | 0.2410 | 0.5035 | 0.0500 |
| discogs-effnet-release hop1 mean-norm rel--20 | 0.1835 | 0.2380 | 0.5025 | 0.0515 |
| openl3 hop0.5 mean nosil | 0.1290 | 0.1695 | 0.3900 | 0.0364 |
| openl3 hop0.5 mean rel--20 | 0.1285 | 0.1685 | 0.3880 | 0.0364 |
| openl3 hop0.5 mean-norm nosil | 0.1280 | 0.1695 | 0.3900 | 0.0364 |
| openl3 hop0.5 mean-norm rel--20 | 0.1290 | 0.1685 | 0.3895 | 0.0364 |
| openl3 hop0.5 robust-mean nosil | 0.1270 | 0.1665 | 0.3940 | 0.0333 |
| openl3 hop0.5 robust-mean rel--20 | 0.1275 | 0.1675 | 0.3945 | 0.0333 |
| openl3 hop1 mean nosil | 0.1290 | 0.1695 | 0.3900 | 0.0364 |
| openl3 hop1 mean rel--20 | 0.1285 | 0.1685 | 0.3880 | 0.0364 |
| openl3 hop1 mean-norm nosil | 0.1280 | 0.1695 | 0.3900 | 0.0364 |
| openl3 hop1 mean-norm rel--20 | 0.1290 | 0.1685 | 0.3895 | 0.0364 |
| openl3 hop1 robust-mean nosil | 0.1270 | 0.1665 | 0.3940 | 0.0333 |
| openl3 hop1 robust-mean rel--20 | 0.1275 | 0.1675 | 0.3945 | 0.0333 |
