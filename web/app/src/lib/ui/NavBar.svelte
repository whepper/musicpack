<!--
Copyright (c) 2026, The MusicPack Development Team
SPDX-License-Identifier: BSD-3-Clause
-->

<script lang="ts">
  import { session, router } from '../bootstrap';

  // Subscribe to the store; a plain router.get() read would freeze the
  // underline on whichever page the bar first rendered with.
  const routeStore = router.route;
  const name = $derived($routeStore.name);
</script>

<header class="topnav">
  <a class="brand" href="/" aria-label="MusicPack home">Music<em>Pack</em></a>
  <nav class="navlinks" aria-label="Primary">
    <a
      class="navlink"
      href="/albums"
      aria-current={name === 'albums' || name === 'album' ? 'page' : undefined}>Albums</a>
    <a
      class="navlink"
      href="/artists"
      aria-current={name === 'artists' || name === 'artist' ? 'page' : undefined}>Artists</a>
    <a
      class="navlink"
      href="/queue"
      aria-current={name === 'queue' ? 'page' : undefined}>Now Playing</a>
  </nav>
  <div style="margin-left:auto;display:flex;align-items:center;gap:var(--space-3)">
    <button class="smallcaps" onclick={() => void session.logout()}>Sign out</button>
  </div>
</header>
