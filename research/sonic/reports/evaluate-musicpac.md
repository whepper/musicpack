# Quantitative evaluation — profile musicpack-sonic-v1-5e6059049aab4cee

Dataset: 100 tracks, 33 albums, 22 artists (aggregate; see raw). Diagnostics, not ground truth — they never define 'similar'.

| pooling/hop/silence | same_album@10 | same_artist@10 | genre_purity@10 | album_coherence@10 |
|---|---|---|---|---|
| clap-music hop10 mean-norm nosil | 0.0930 | 0.1360 | 0.2700 | 0.0382 |
| discogs-effnet-multi hop1 mean-norm nosil | 0.2010 | 0.3160 | 0.5460 | 0.0485 |
| discogs-effnet-multi hop1 mean-norm rel--20 | 0.2010 | 0.3160 | 0.5480 | 0.0485 |
| discogs-effnet-release hop1 mean-norm nosil | 0.2060 | 0.3020 | 0.5140 | 0.0500 |
| discogs-effnet-release hop1 mean-norm rel--20 | 0.2070 | 0.3030 | 0.5140 | 0.0515 |
| openl3 hop0.5 mean nosil | 0.1520 | 0.2250 | 0.3570 | 0.0364 |
| openl3 hop0.5 mean rel--20 | 0.1530 | 0.2230 | 0.3600 | 0.0364 |
| openl3 hop0.5 mean-norm nosil | 0.1520 | 0.2230 | 0.3550 | 0.0364 |
| openl3 hop0.5 mean-norm rel--20 | 0.1530 | 0.2230 | 0.3580 | 0.0364 |
| openl3 hop0.5 robust-mean nosil | 0.1490 | 0.2190 | 0.3580 | 0.0333 |
| openl3 hop0.5 robust-mean rel--20 | 0.1530 | 0.2240 | 0.3610 | 0.0333 |
| openl3 hop1 mean nosil | 0.1520 | 0.2250 | 0.3570 | 0.0364 |
| openl3 hop1 mean rel--20 | 0.1530 | 0.2230 | 0.3600 | 0.0364 |
| openl3 hop1 mean-norm nosil | 0.1520 | 0.2230 | 0.3550 | 0.0364 |
| openl3 hop1 mean-norm rel--20 | 0.1530 | 0.2230 | 0.3580 | 0.0364 |
| openl3 hop1 robust-mean nosil | 0.1490 | 0.2190 | 0.3580 | 0.0333 |
| openl3 hop1 robust-mean rel--20 | 0.1530 | 0.2240 | 0.3610 | 0.0333 |
