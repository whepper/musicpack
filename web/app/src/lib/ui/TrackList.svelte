<!--
Copyright (c) 2026, The MusicPack Development Team
SPDX-License-Identifier: BSD-3-Clause
-->

<script lang="ts">
  import type { ReleaseDetail } from '../api/types';
  import { fmtTime, mediumLabel } from '../format';

  let {
    release,
    currentTrackId,
    onPlay,
  }: {
    release: ReleaseDetail;
    currentTrackId?: number;
    onPlay: (index: number) => void;
  } = $props();

  const discs = $derived([...release.media].sort((a, b) => a.disc - b.disc));
  // Flattened queue index of the first track of each disc, so clicking a
  // disc-2 track selects the correct album-absolute queue index (not the
  // disc-local track number).
  const discStarts = $derived.by(() => {
    const starts: number[] = [];
    let acc = 0;
    for (const d of discs) {
      starts.push(acc);
      acc += d.tracks.length;
    }
    return starts;
  });
</script>

<div class="tracklist">
  {#each discs as disc, di (disc.disc)}
    {#if discs.length > 1}
      <h3 class="disc-title">
        Disc {disc.disc}
        {#if disc.title}<span class="muted">— {disc.title}</span>{/if}
      </h3>
    {/if}
    <div role="list" aria-label={`Disc ${disc.disc} tracks`}>
      {#each disc.tracks as track, ti (track.id)}
        <button
          class="track"
          aria-current={track.id === currentTrackId ? 'true' : undefined}
          onclick={() => onPlay((discStarts[di] ?? 0) + ti)}
        >
          <span class="num">{track.number}</span>
          <span class="tt">{track.title}</span>
          <span class="dur">{track.duration ? fmtTime(track.duration) : '—'}</span>
        </button>
      {/each}
    </div>
  {/each}
</div>
