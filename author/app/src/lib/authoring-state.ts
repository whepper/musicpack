// Copyright (c) 2026, The MusicPack Development Team
// SPDX-License-Identifier: BSD-3-Clause

// Shared authoring-session state (validation result, create flow, encode
// staging) shared by the album view and the fixed footer/status bar. Plain
// TS + writable.

import { writable } from './store';
import type { CreateResult, ValidationResult } from './types';

export const validation = writable<ValidationResult | null>(null);
export const validating = writable<boolean>(false);
export const createOpen = writable<boolean>(false);
export const createResult = writable<CreateResult | null>(null);

/** The encode staging directory for the current draft (set after a successful
 * encode, cleared after the package build or when a new album is loaded). */
export const encodeStaging = writable<string | null>(null);

export function setEncodeStaging(dir: string | null): void {
  encodeStaging.set(dir);
}

export function invalidateValidation(): void {
  validation.set(null);
}
