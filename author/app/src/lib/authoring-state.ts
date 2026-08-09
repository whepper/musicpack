// Shared authoring-session state (validation result, create flow) shared by
// the album view and the fixed footer/status bar. Plain TS + writable.

import { writable } from './store';
import type { CreateResult, ValidationResult } from './types';

export const validation = writable<ValidationResult | null>(null);
export const validating = writable<boolean>(false);
export const createOpen = writable<boolean>(false);
export const createResult = writable<CreateResult | null>(null);
