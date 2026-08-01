import { readFile } from 'node:fs/promises';
import { describe, expect, it } from 'vitest';
import { getCoverPlaceholder } from '../../src/lib/coverPlaceholder';

const testFileUrl = import.meta.url;

describe('getCoverPlaceholder', () => {
  it('同一安全正整数始终返回同一本地 WebP', () => {
    expect(getCoverPlaceholder(29)).toBe(getCoverPlaceholder(29));
    expect(getCoverPlaceholder(29)).toMatch(/^\/covers\/crystal-cover-0[1-4]\.webp$/);
  });

  it('按 music_id % 4 稳定轮换四张封面', () => {
    expect([4, 1, 2, 3].map(getCoverPlaceholder)).toEqual([
      '/covers/crystal-cover-01.webp',
      '/covers/crystal-cover-02.webp',
      '/covers/crystal-cover-03.webp',
      '/covers/crystal-cover-04.webp',
    ]);
  });

  it.each([0, -1, 1.5, Number.MAX_SAFE_INTEGER + 1, Number.NaN, Number.POSITIVE_INFINITY])(
    '无效 ID %s 返回第一张封面',
    (musicId) => expect(getCoverPlaceholder(musicId)).toBe('/covers/crystal-cover-01.webp'),
  );

  it('四个静态文件都是非空 WebP', async () => {
    for (let index = 1; index <= 4; index += 1) {
      const bytes = await readFile(new URL(`../../public/covers/crystal-cover-0${index}.webp`, testFileUrl));
      expect(bytes.byteLength).toBeGreaterThan(4096);
      expect(bytes.subarray(0, 4).toString('ascii')).toBe('RIFF');
      expect(bytes.subarray(8, 12).toString('ascii')).toBe('WEBP');
    }
  });
});
