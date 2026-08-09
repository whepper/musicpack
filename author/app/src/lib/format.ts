// Small pure formatting helpers (shared style with the web client's
// lib/format.ts — no date/time library).

export function fmtTime(seconds?: number): string {
  if (typeof seconds !== 'number' || !Number.isFinite(seconds) || seconds < 0) {
    return '–';
  }
  const total = Math.round(seconds);
  const m = Math.floor(total / 60);
  const s = total % 60;
  return `${m}:${String(s).padStart(2, '0')}`;
}

export function yearOf(date?: string): string {
  if (!date) return '';
  return date.slice(0, 4);
}

export function formatDate(date?: string): string {
  if (!date) return '';
  const [y, m, d] = date.split('-');
  if (!y) return '';
  if (!m) return y;
  if (!d) {
    const month = new Date(Number(y), Number(m) - 1, 1).toLocaleString('en', {
      month: 'long',
    });
    return `${month} ${y}`;
  }
  const month = new Date(Number(y), Number(m) - 1, 1).toLocaleString('en', {
    month: 'short',
  });
  return `${d} ${month} ${y}`;
}

export function artistLine(artists: { name: string; role?: string }[]): string {
  return artists
    .map((a) => (a.role && a.role !== 'main' ? `${a.name} (${a.role})` : a.name))
    .join(', ');
}

export function codecLabel(codec?: string, version?: number): string {
  if (!codec) return '?';
  if (codec.startsWith('musepack')) return version === 8 ? 'MPC' : 'MPC';
  if (codec === 'flac') return 'FLAC';
  if (codec === 'wav') return 'WAV';
  if (codec === 'ogg') return 'OGG';
  return codec.toUpperCase();
}
