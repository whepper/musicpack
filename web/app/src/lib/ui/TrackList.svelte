<!--
Copyright (c) 2026, The MusicPack Development Team
SPDX-License-Identifier: BSD-3-Clause
-->

<script lang="ts">
  import type { ReleaseDetail, Track } from '../api/types';
  import { fmtTime, mediumLabel, codecLabel } from '../format';
  import { queue } from '../bootstrap';
  import { itemForTrack } from '../state/queue';

  let {
    release,
    currentTrackId,
    onPlay,
  }: {
    release: ReleaseDetail;
    currentTrackId?: number;
    onPlay: (index: number) => void;
  } = $props();

  let addedSet = $state<Set<number>>(new Set());

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

  function addTrack(track: Track): void {
    const artist = release.album.artists[0]?.name ?? '';
    queue.addItem(itemForTrack(release, track, release.album.title, artist));
    addedSet = new Set([...addedSet, track.id]);
    setTimeout(() => {
      addedSet = new Set([...addedSet].filter((id) => id !== track.id));
    }, 1200);
  }
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
        <div
          class="track"
          class:now-playing={track.id === currentTrackId}
          aria-current={track.id === currentTrackId ? 'true' : undefined}
        >
          <button
            class="track-play"
            onclick={() => onPlay((discStarts[di] ?? 0) + ti)}
          >
            <span class="num">{track.number}</span>
            <span class="tt">{track.title}</span>
            <span class="meta">
              <span class="dur">{track.duration ? fmtTime(track.duration) : '—'}</span>
              {#if codecLabel(track.codec.codec)}
                <span class="codec">{codecLabel(track.codec.codec)}</span>
              {/if}
            </span>
          </button>
          <button
            class="queue-add"
            aria-label={`Add ${track.title} to queue`}
            onclick={() => addTrack(track)}
          >{addedSet.has(track.id) ? '✓' : '+'}</button>
        </div>
      {/each}
    </div>
  {/each}
</div>
