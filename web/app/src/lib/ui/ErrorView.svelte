<!--
Copyright (c) 2026, The MusicPack Development Team
SPDX-License-Identifier: BSD-3-Clause
-->

<script lang="ts">
  let { message, detail }: { message: string; detail?: string } = $props();
</script>

<div role="alert" class="empty-state" style="font-family:var(--sans);font-size:var(--fs-md)">
  <p style="margin:0 0 var(--space-2)">{message}</p>
  {#if detail}
    <p class="muted" style="font-size:var(--fs-sm);margin:0">{detail}</p>
  {/if}
</div>
