const coverPlaceholders = [
  '/covers/crystal-cover-01.webp',
  '/covers/crystal-cover-02.webp',
  '/covers/crystal-cover-03.webp',
  '/covers/crystal-cover-04.webp',
] as const;

export function getCoverPlaceholder(musicId: number): string {
  if (!Number.isSafeInteger(musicId) || musicId <= 0) return coverPlaceholders[0];
  return coverPlaceholders[musicId % coverPlaceholders.length];
}
