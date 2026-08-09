<script lang="ts">
  import { onMount } from 'svelte';
  import { getCurrentWebviewWindow } from '@tauri-apps/api/webviewWindow';
  import { api } from '../bootstrap';

  let { onOpen }: { onOpen: (path: string) => void } = $props();
  let over = $state(false);

  onMount(() => {
    // Tauri delivers drag/drop as an event with absolute file paths; a plain
    // HTML5 drop cannot reveal full paths. Guard for non-Tauri contexts.
    try {
      const unlisten = getCurrentWebviewWindow().onDragDropEvent((event) => {
        if (event.payload.type === 'over') {
          over = true;
        } else if (event.payload.type === 'drop') {
          over = false;
          const first = event.payload.paths[0];
          if (first) onOpen(first);
        } else {
          over = false;
        }
      });
      return () => {
        void unlisten.then((fn) => fn());
      };
    } catch {
      return undefined;
    }
  });

  async function choose(): Promise<void> {
    const dir = await api.pickDirectory();
    if (dir) onOpen(dir);
  }
</script>

<section class="welcome">
  <h1>Author an album</h1>
  <p class="muted">
    Turn a tagged Musepack album — the output of <em>flac2mpc</em> — into a curated
    <span class="smallcaps">.mpack</span> release.
  </p>
  <div
    class="drop"
    class:over
    role="button"
    tabindex="0"
    ondragover={(e) => e.preventDefault()}
    ondrop={(e) => e.preventDefault()}
    onkeydown={(e) => {
      if (e.key === 'Enter') void choose();
    }}
  >
    <p>Drop an album directory here, or</p>
    <button class="btn" onclick={choose}>Choose album…</button>
  </div>
</section>
