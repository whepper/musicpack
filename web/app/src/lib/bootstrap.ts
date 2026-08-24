// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

// Application composition root: wires the API client, session, stores,
// playback controller and router into singletons the Svelte components use.
import { ApiClient } from './api/client';
import { bindSession, type SessionStore } from './auth/session';
import { createLibraryStore, type LibraryStore } from './state/library';
import { createQueueStore, type QueueStore } from './state/queue';
import { PlayerController } from './playback/controller';
import { createTransitionPlanner } from './playback/transition-profiles';
import { createRouter, type Router } from './router';

export const api = new ApiClient({});
export const session: SessionStore = bindSession(api);
export const library: LibraryStore = createLibraryStore(api);
export const queue: QueueStore = createQueueStore();
// Content-aware transitions: profiles come from the tracks' waveform
// envelopes; without data the planner degrades to the legacy fixed fade.
const transitionPlanner = createTransitionPlanner({
  base: api.baseUrl,
  token: () => api.getToken(),
});
export const player = new PlayerController(queue, {
  planTransition: (query) => transitionPlanner.plan(query),
});
// Prefetch boundary profiles for the current and next item as playback
// advances so plans are content-aware by the time a boundary approaches.
player.on((event) => {
  if (event.t !== 'track') return;
  const q = queue.get();
  const current = q.items[q.index] ?? null;
  if (current) transitionPlanner.prime(current);
  const next = q.items[q.index + 1] ?? null;
  if (next) transitionPlanner.prime(next);
});
/** The player model as a store (subscribe via `$playerModel`). */
export const playerModel = player.model;
export const router: Router = createRouter();

// Stop playback and dispose the backend when the session ends (sign-out or
// expiry), so audio and Media Session state never leak across the auth
// boundary. The controller rebuilds its backend on the next play.
{
  let wasAuthenticated = false;
  session.subscribe((m) => {
    if (wasAuthenticated && m.state !== 'authenticated') {
      void player.teardown();
      queue.clear();
    }
    wasAuthenticated = m.state === 'authenticated';
  });
}

/** Test/dev instrumentation exposed for Playwright + perf reporting. */
export interface MusicPackDebug {
  api: ApiClient;
  session: SessionStore;
  library: LibraryStore;
  queue: QueueStore;
  player: PlayerController;
  router: Router;
}

declare global {
  interface Window {
    __musicpack?: MusicPackDebug;
  }
}

export function exposeDebug(): void {
  window.__musicpack = { api, session, library, queue, player, router };
}
