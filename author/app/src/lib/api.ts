// The Author API: a thin typed facade over the Tauri command surface.
//
// The `musicpack` CLI is only ever invoked by the Rust `AuthorService`; the
// frontend talks to it through these commands and never parses CLI text.
// Every interaction is structured JSON. The invoke function is injectable so
// tests can substitute a fake (mirroring the web client's ApiClient design).

import { invoke } from '@tauri-apps/api/core';
import { open } from '@tauri-apps/plugin-dialog';
import { revealItemInDir } from '@tauri-apps/plugin-opener';
import type {
  BackendInfo,
  CreateResult,
  Draft,
  IdentifyOptions,
  IdentifyResult,
  ReadImageResult,
  ValidationResult,
} from './types';

export type InvokeFn = (
  cmd: string,
  args?: Record<string, unknown>,
) => Promise<unknown>;

export interface PluginFacade {
  pickDirectory(): Promise<string | null>;
  pickImageFile(): Promise<string | null>;
  pickOutputDirectory(): Promise<string | null>;
  revealInFinder(path: string): Promise<void>;
}

async function defaultPickDirectory(): Promise<string | null> {
  return (await open({ directory: true })) ?? null;
}

async function defaultPickImageFile(): Promise<string | null> {
  const picked = await open({
    multiple: false,
    filters: [
      { name: 'Images', extensions: ['jpg', 'jpeg', 'png', 'gif', 'webp', 'bmp'] },
    ],
  });
  return typeof picked === 'string' ? picked : null;
}

async function defaultPickOutputDirectory(): Promise<string | null> {
  return (await open({ directory: true })) ?? null;
}

async function defaultRevealInFinder(path: string): Promise<void> {
  await revealItemInDir(path);
}

export class AuthorApi {
  constructor(
    private invokeFn: InvokeFn = invoke,
    private plugins: PluginFacade = {
      pickDirectory: defaultPickDirectory,
      pickImageFile: defaultPickImageFile,
      pickOutputDirectory: defaultPickOutputDirectory,
      revealInFinder: defaultRevealInFinder,
    },
  ) {}

  async backendInfo(): Promise<BackendInfo> {
    return (await this.invokeFn('backend_info', {})) as BackendInfo;
  }

  async inspectAlbum(path: string): Promise<Draft> {
    return (await this.invokeFn('inspect_album', { path })) as Draft;
  }

  async validateDraft(draft: Draft): Promise<ValidationResult> {
    return (await this.invokeFn('validate_draft', {
      draftJson: JSON.stringify(draft),
    })) as ValidationResult;
  }

  async identifyDraft(draft: Draft, opts: IdentifyOptions): Promise<IdentifyResult> {
    return (await this.invokeFn('identify_draft', {
      draftJson: JSON.stringify(draft),
      mbid: opts.mbid ?? null,
      barcode: opts.barcode ?? null,
      mbJson: opts.mbJson ?? null,
    })) as IdentifyResult;
  }

  async createPackage(draft: Draft, outputDir: string): Promise<CreateResult> {
    return (await this.invokeFn('create_package', {
      draftJson: JSON.stringify(draft),
      outputDir,
    })) as CreateResult;
  }

  async verifyPackage(path: string): Promise<ValidationResult> {
    return (await this.invokeFn('verify_package', { path })) as ValidationResult;
  }

  async readImage(path: string): Promise<ReadImageResult> {
    return (await this.invokeFn('read_image', { path })) as ReadImageResult;
  }

  pickDirectory(): Promise<string | null> {
    return this.plugins.pickDirectory();
  }

  pickImageFile(): Promise<string | null> {
    return this.plugins.pickImageFile();
  }

  pickOutputDirectory(): Promise<string | null> {
    return this.plugins.pickOutputDirectory();
  }

  revealInFinder(path: string): Promise<void> {
    return this.plugins.revealInFinder(path);
  }
}
