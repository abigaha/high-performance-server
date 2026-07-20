import { describe, it, expect, vi, beforeEach } from 'vitest';

const mockFetch = vi.fn();
globalThis.fetch = mockFetch;

beforeEach(() => {
  mockFetch.mockReset();
  localStorage.setItem('token', 'test-token');
});

describe('files API', () => {
  it('getFiles returns paginated file list', async () => {
    const items = [
      { file_id: 1, file_name: 'test.mp3', file_hash: 'abc', file_size: 1000, content_type: 'audio/mpeg', created_at: '2024-01-01' },
    ];
    mockFetch.mockResolvedValueOnce({
      ok: true,
      json: async () => ({ items, total: 1, offset: 0, limit: 20 }),
    });

    const { getFiles } = await import('../../src/api/files');
    const res = await getFiles({ offset: 0, limit: 20 });

    expect(mockFetch).toHaveBeenCalledWith(
      expect.stringContaining('/api/files'),
      expect.anything(),
    );
    expect(res.items).toHaveLength(1);
    expect(res.items[0].file_name).toBe('test.mp3');
  });

  it('deleteFile calls DELETE method', async () => {
    mockFetch.mockResolvedValueOnce({ ok: true, json: async () => {} });

    const { deleteFile } = await import('../../src/api/files');
    await deleteFile(1);

    expect(mockFetch).toHaveBeenCalledWith(
      expect.stringContaining('/api/files/1'),
      expect.objectContaining({ method: 'DELETE' }),
    );
  });
});
