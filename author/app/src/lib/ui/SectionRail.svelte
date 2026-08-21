<!--
Copyright (c) 2026, The MusicPack Development Team
SPDX-License-Identifier: BSD-3-Clause
-->

<!-- Sticky section rail: anchor navigation over the album page with a status
     dot per section, doubling as an at-a-glance progress tracker. -->
<script lang="ts">
  import { onMount } from 'svelte';
  import { draft } from '../bootstrap';
  import { chipState } from '../draft-store';

  interface Section {
    id: string;
    label: string;
    key: keyof ReturnType<typeof chipState> | null;
  }

  const SECTIONS: Section[] = [
    { id: 'sec-identity', label: 'Identity', key: 'identity' },
    { id: 'sec-release', label: 'Release', key: 'metadata' },
    { id: 'sec-tracks', label: 'Tracks', key: 'audio' },
    { id: 'sec-artwork', label: 'Artwork', key: 'artwork' },
    { id: 'sec-encode', label: 'Encode', key: null },
    { id: 'sec-sonic', label: 'Sonic', key: 'sonic' },
    { id: 'sec-waveform', label: 'Waveform', key: 'waveform' },
    { id: 'sec-validation', label: 'Validate', key: null },
  ];

  let active = $state('sec-identity');
  let observer: IntersectionObserver | null = null;

  function dotClass(key: Section['key']): string {
    if (!key || !$draft) return '';
    return chipState($draft)[key];
  }

  onMount(() => {
    observer = new IntersectionObserver(
      (entries) => {
        for (const e of entries) {
          if (e.isIntersecting) active = e.target.id;
        }
      },
      // a narrow band around the upper third of the viewport decides which
      // section counts as "current"
      { rootMargin: '-15% 0px -70% 0px', threshold: 0 },
    );
    for (const s of SECTIONS) {
      const el = document.getElementById(s.id);
      if (el) observer.observe(el);
    }
    return () => observer?.disconnect();
  });
</script>

<nav class="rail" aria-label="Album sections">
  {#each SECTIONS as s}
    <a href="#{s.id}" class="rail-link" class:active={active === s.id}>
      <span class="dot {dotClass(s.key)}" aria-hidden="true"></span>
      <span class="label">{s.label}</span>
    </a>
  {/each}
</nav>

<style>
  .rail {
    position: sticky;
    top: 84px;
    align-self: start;
    display: flex;
    flex-direction: column;
    gap: 2px;
    width: 128px;
    flex-shrink: 0;
  }
  .rail-link {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 4px 10px;
    border-radius: 5px;
    text-decoration: none;
    color: var(--ink-faint);
    font-size: 12px;
    line-height: 1.6;
  }
  .rail-link:hover {
    background: var(--paper-raised);
    color: var(--ink);
  }
  .rail-link.active {
    color: var(--ink);
    font-weight: 600;
  }
  .dot {
    width: 7px;
    height: 7px;
    border-radius: 50%;
    background: var(--hairline, rgba(0, 0, 0, 0.18));
    flex-shrink: 0;
  }
  .dot.ok {
    background: #3f8c5a;
  }
  .dot.warn {
    background: #c07a3a;
  }
  .dot.idle {
    background: var(--ink-faint);
    opacity: 0.55;
  }
</style>
