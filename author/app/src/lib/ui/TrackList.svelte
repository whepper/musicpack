<script lang="ts">
  import { draft, draftStore } from '../bootstrap';
  import { codecLabel, fmtTime } from '../format';
  import type { Track } from '../types';


  let editing = $state<{ disc: number; track: number } | null>(null);

  function beginEdit(disc: number, track: number): void {
    editing = { disc, track };
  }

  function commitEdit(disc: number, index: number, patch: Partial<Track>): void {
    draftStore.updateTrack(disc, index, patch);
    editing = null;
  }

  function updateTrackField(disc: number, index: number, patch: Partial<Track>): void {
    draftStore.updateTrack(disc, index, patch);
  }
</script>

{#if $draft}
{#each $draft.media as medium, di}
  <h3 class="disc-title">
    Disc {medium.disc}
    {#if medium.format}<span class="smallcaps">{medium.format}</span>{/if}
    {#if medium.title}— {medium.title}{/if}
  </h3>
  <div class="tracklist">
    {#each medium.tracks as track, ti}
      {@const isEditing = editing?.disc === di && editing?.track === ti}
      {#if isEditing}
        <div class="track editing">
          <span class="num">{track.track}</span>
          <input
            type="text"
            aria-label="Track title"
            value={track.title}
            oninput={(e) => updateTrackField(di, ti, { title: e.currentTarget.value })}
          />
          <input
            type="text"
            aria-label="Track artist"
            placeholder="artist"
            value={track.artists?.[0]?.name ?? ''}
            oninput={(e) =>
              draftStore.updateTrack(di, ti, {
                artists: e.currentTarget.value
                  ? [{ name: e.currentTarget.value, role: 'main' }]
                  : undefined,
              })}
          />
          <button class="btn ghost" onclick={() => commitEdit(di, ti, {})}>Done</button>
        </div>
      {:else}
        <button class="track" onclick={() => beginEdit(di, ti)}>
          <span class="num">{track.track}</span>
          <span class="tt">
            {#if track.title}{track.title}{:else}<em>untitled</em>{/if}
            {#if track.artists?.[0]}
              <span class="smallcaps"> · {track.artists[0].name}</span>
            {/if}
          </span>
          <span class="dur">{fmtTime(track.duration)}</span>
          <span class="codec-tag">{codecLabel(track.codec, track.streamVersion)}</span>
          {#if track.sampleRate}
            <span class="smallcaps">
              {track.sampleRate / 1000} kHz{track.bitDepth ? ` · ${track.bitDepth}-bit` : ''}
            </span>
          {/if}
          <span class="filename">{track.audioPath}</span>
          {#if track.identifiers?.isrc}<span class="smallcaps">ISRC {track.identifiers.isrc}</span>{/if}
          {#if track.identifiers?.musicbrainzRecordingId}<span class="smallcaps">MB {track.identifiers.musicbrainzRecordingId.slice(0, 8)}</span>{/if}
        </button>
      {/if}
    {/each}
  </div>
{/each}
{/if}
