import { describe, expect, it } from 'vitest';
import { formatBytes, formatDate } from './format';

describe('formatBytes', () => {
  it('formats zero and small byte counts', () => {
    expect(formatBytes(0)).toBe('0 B');
    expect(formatBytes(512)).toBe('512 B');
  });

  it('uses one decimal for small fractional units, none for large', () => {
    expect(formatBytes(1500)).toBe('1.5 KB');
    expect(formatBytes(2_300_000)).toBe('2.2 MB');
    expect(formatBytes(20 * 1024)).toBe('20 KB'); // >=10 -> no decimals
  });

  it('returns an em dash for invalid input', () => {
    expect(formatBytes(-1)).toBe('—');
    expect(formatBytes(NaN)).toBe('—');
    expect(formatBytes(Infinity)).toBe('—');
  });
});

describe('formatDate', () => {
  it('returns an em dash for undefined or invalid dates', () => {
    expect(formatDate(undefined)).toBe('—');
    expect(formatDate('not-a-date')).toBe('—');
  });

  it('renders a valid date to a non-empty string', () => {
    const out = formatDate('2026-06-17T12:00:00Z');
    expect(out).not.toBe('—');
    expect(out.length).toBeGreaterThan(0);
  });
});
