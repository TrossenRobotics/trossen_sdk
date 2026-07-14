export function formatDate(value: string | Date | undefined): string {
    if (value === undefined) return '—';
    const date = typeof value === 'string' ? new Date(value) : value;
    if (isNaN(date.getTime())) return '—';
    return date.toLocaleString();
}

/**
   * Format a byte count as a human-readable string.
   *
   * Examples:
   *   formatBytes(0)         → '0 B'
   *   formatBytes(512)       → '512 B'
   *   formatBytes(1500)      → '1.5 KB'
   *   formatBytes(2_300_000) → '2.2 MB'
   *
   * Uses base-1024 (binary) units, which match what file managers show.
   * Switch to base-1000 (decimal) here if you ever need vendor-style sizes.
   */
export function formatBytes(bytes: number): string {
    if (bytes === 0) return '0 B';
    if (!Number.isFinite(bytes) || bytes < 0) return '—';
    const units = ['B', 'KB', 'MB', 'GB', 'TB', 'PB'];
    const i = Math.min(
        units.length - 1,
        Math.floor(Math.log(bytes) / Math.log(1024)),
    );
    const value = bytes / Math.pow(1024, i);
    // 1 decimal place for small values to avoid showing "1 KB" when it's
    // really 1.5 KB; whole numbers for >=10 because the precision rarely
    // matters at that scale.
    const decimals = value < 10 && i > 0 ? 1 : 0;
    return `${value.toFixed(decimals)} ${units[i]}`;
}
