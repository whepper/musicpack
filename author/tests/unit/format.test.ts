import { describe, it, expect } from 'vitest';
import { fmtTime, yearOf, formatDate, artistLine, codecLabel } from '../../app/src/lib/format';

describe('format helpers', () => {
  it('formats durations as M:SS', () => {
    expect(fmtTime(252.3)).toBe('4:12');
    expect(fmtTime(61)).toBe('1:01');
    expect(fmtTime(0)).toBe('0:00');
    expect(fmtTime(-1)).toBe('–');
    expect(fmtTime(undefined)).toBe('–');
  });

  it('formats dates and years', () => {
    expect(yearOf('1986-06-16')).toBe('1986');
    expect(formatDate('1986-06-16')).toBe('16 Jun 1986');
    expect(formatDate('1986')).toBe('1986');
    expect(formatDate('')).toBe('');
  });

  it('joins artist credits', () => {
    expect(artistLine([{ name: 'A', role: 'main' }, { name: 'B', role: 'featuring' }])).toBe(
      'A, B (featuring)',
    );
  });

  it('labels codecs', () => {
    expect(codecLabel('musepack-sv8', 8)).toBe('MPC');
    expect(codecLabel('flac')).toBe('FLAC');
    expect(codecLabel()).toBe('?');
  });
});
