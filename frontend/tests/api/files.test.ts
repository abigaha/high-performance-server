import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { ApiError, injectUnauthorizedHandler, markSessionChanged } from '../../src/api/client';

const mockFetch = vi.fn();
globalThis.fetch = mockFetch;

class MockXMLHttpRequest {
  static instances: MockXMLHttpRequest[] = [];

  method = '';
  url = '';
  status = 0;
  responseText = '';
  requestHeaders: Record<string, string> = {};
  sentBody: XMLHttpRequestBodyInit | null = null;
  upload = { onprogress: null as ((event: ProgressEvent) => void) | null };
  onload: ((event: ProgressEvent) => void) | null = null;
  onerror: ((event: ProgressEvent) => void) | null = null;
  onabort: ((event: ProgressEvent) => void) | null = null;

  constructor() {
    MockXMLHttpRequest.instances.push(this);
  }

  open(method: string, url: string) {
    this.method = method;
    this.url = url;
  }

  setRequestHeader(name: string, value: string) {
    this.requestHeaders[name] = value;
  }

  send(body: XMLHttpRequestBodyInit | null) {
    this.sentBody = body;
  }

  abort() {
    this.onabort?.(new ProgressEvent('abort'));
  }

  respond(status: number, responseText: string) {
    this.status = status;
    this.responseText = responseText;
    this.onload?.(new ProgressEvent('load'));
  }
}

globalThis.XMLHttpRequest = MockXMLHttpRequest as unknown as typeof XMLHttpRequest;

beforeEach(() => {
  mockFetch.mockReset();
  MockXMLHttpRequest.instances = [];
  localStorage.clear();
  localStorage.setItem('token', 'test-token');
  injectUnauthorizedHandler(() => {
    localStorage.removeItem('token');
    markSessionChanged();
  });
  window.history.replaceState({}, '', '/login');
});

afterEach(() => {
  injectUnauthorizedHandler(null);
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

  it('download fetches a Blob with the bearer token', async () => {
    const blob = new Blob(['audio'], { type: 'audio/mpeg' });
    mockFetch.mockResolvedValueOnce({ ok: true, blob: async () => blob });
    const createObjectURL = vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:test');

    const { getFileDownloadUrl } = await import('../../src/api/files');
    await expect(getFileDownloadUrl(8)).resolves.toBe('blob:test');

    expect(mockFetch).toHaveBeenCalledWith(
      expect.stringContaining('/api/files/8/download'),
      expect.objectContaining({
        headers: expect.objectContaining({ Authorization: 'Bearer test-token' }),
      }),
    );
    expect(createObjectURL).toHaveBeenCalledWith(blob);
    createObjectURL.mockRestore();
  });

  it('download aborts before a late response can create a Blob URL', async () => {
    let resolveResponse!: (response: { ok: boolean; blob: () => Promise<Blob> }) => void;
    const response = {
      ok: true,
      blob: vi.fn(async () => new Blob(['late download'], { type: 'audio/mpeg' })),
    };
    mockFetch.mockReturnValueOnce(new Promise((resolve) => {
      resolveResponse = resolve;
    }));
    const createObjectURL = vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:late-download');
    const controller = new AbortController();

    const { getFileDownloadUrl } = await import('../../src/api/files');
    const operation = getFileDownloadUrl(8, controller.signal);
    controller.abort();
    resolveResponse(response);

    await expect(operation).rejects.toEqual(expect.objectContaining({ name: 'AbortError' }));
    expect(response.blob).not.toHaveBeenCalled();
    expect(createObjectURL).not.toHaveBeenCalled();
    createObjectURL.mockRestore();
  });

  it('download stops before Blob parsing when its captured session becomes stale', async () => {
    let resolveResponse!: (response: { ok: boolean; blob: () => Promise<Blob> }) => void;
    const response = {
      ok: true,
      blob: vi.fn(async () => new Blob(['late download'], { type: 'audio/mpeg' })),
    };
    mockFetch.mockReturnValueOnce(new Promise((resolve) => {
      resolveResponse = resolve;
    }));
    const createObjectURL = vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:stale-session-download');

    const { getFileDownloadUrl } = await import('../../src/api/files');
    const operation = getFileDownloadUrl(8);
    markSessionChanged();
    resolveResponse(response);

    await expect(operation).rejects.toEqual(expect.objectContaining({ name: 'AbortError' }));
    expect(response.blob).not.toHaveBeenCalled();
    expect(createObjectURL).not.toHaveBeenCalled();
    createObjectURL.mockRestore();
  });

  it('stream fetches an authenticated audio Blob and returns an object URL', async () => {
    const blob = new Blob(['stream-audio'], { type: 'audio/flac' });
    mockFetch.mockResolvedValueOnce({ ok: true, blob: async () => blob });
    const createObjectURL = vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:stream');

    const { getFileStreamUrl } = await import('../../src/api/files');
    await expect(getFileStreamUrl(15)).resolves.toBe('blob:stream');

    expect(mockFetch).toHaveBeenCalledWith(
      expect.stringContaining('/api/files/15/stream'),
      expect.objectContaining({
        headers: expect.objectContaining({
          Accept: 'audio/*',
          Authorization: 'Bearer test-token',
        }),
      }),
    );
    expect(createObjectURL).toHaveBeenCalledWith(blob);
    createObjectURL.mockRestore();
  });

  it('upload sends the raw file and RFC5987 filename', async () => {
    const { uploadFile } = await import('../../src/api/files');
    const file = new File(['audio'], '音乐.mp3', { type: 'audio/mpeg' });

    const result = uploadFile(file);
    const xhr = MockXMLHttpRequest.instances[0];

    expect(xhr.method).toBe('POST');
    expect(xhr.sentBody).toBe(file);
    expect(xhr.requestHeaders.Authorization).toBe('Bearer test-token');
    expect(xhr.requestHeaders['Content-Type']).toBe('audio/mpeg');
    expect(xhr.requestHeaders['Content-Disposition']).toContain("filename*=UTF-8''%E9%9F%B3%E4%B9%90.mp3");

    xhr.respond(201, JSON.stringify({
      file_id: 1,
      file_name: '音乐.mp3',
      file_hash: 'hash',
      size: 5,
      chunks: 1,
    }));
    await expect(result).resolves.toEqual(expect.objectContaining({ file_id: 1, size: 5 }));
  });

  it('upload preserves JSON and HTML error details', async () => {
    const { uploadFile } = await import('../../src/api/files');
    const file = new File(['audio'], 'music.mp3', { type: 'audio/mpeg' });

    const jsonResult = uploadFile(file);
    MockXMLHttpRequest.instances[0].respond(415, '{"error":"文件签名与扩展名不匹配"}');
    await expect(jsonResult).rejects.toEqual(
      expect.objectContaining({ status: 415, message: '文件签名与扩展名不匹配' }),
    );

    const htmlResult = uploadFile(file);
    MockXMLHttpRequest.instances[1].respond(413, '<html><body><h1>请求体过大</h1></body></html>');
    await expect(htmlResult).rejects.toEqual(
      expect.objectContaining({ status: 413, message: '请求体过大' }),
    );
  });

  it('upload 将仅含 code 的 JSON 映射为与 request 一致的 ApiError', async () => {
    const { uploadFile } = await import('../../src/api/files');
    const file = new File(['audio'], 'music.mp3', { type: 'audio/mpeg' });

    const result = uploadFile(file);
    MockXMLHttpRequest.instances[0].respond(400, '{"code":"INVALID_UPLOAD"}');

    await expect(result).rejects.toBeInstanceOf(ApiError);
    await expect(result).rejects.toMatchObject({
      status: 400,
      code: 'INVALID_UPLOAD',
      message: '{"code":"INVALID_UPLOAD"}',
    });
  });

  it('upload 保留 error 或 message 的详细信息及后端 code', async () => {
    const { uploadFile } = await import('../../src/api/files');
    const file = new File(['audio'], 'music.mp3', { type: 'audio/mpeg' });

    const errorResult = uploadFile(file);
    MockXMLHttpRequest.instances[0].respond(415, '{"code":"TYPE_MISMATCH","error":"文件签名与扩展名不匹配"}');
    await expect(errorResult).rejects.toMatchObject({
      status: 415,
      code: 'TYPE_MISMATCH',
      message: '文件签名与扩展名不匹配',
    });

    const messageResult = uploadFile(file);
    MockXMLHttpRequest.instances[1].respond(413, '{"code":"PAYLOAD_TOO_LARGE","message":"上传内容过大"}');
    await expect(messageResult).rejects.toMatchObject({
      status: 413,
      code: 'PAYLOAD_TOO_LARGE',
      message: '上传内容过大',
    });
  });

  it('upload 按 request 的规则提取非 JSON 错误正文', async () => {
    const { uploadFile } = await import('../../src/api/files');
    const file = new File(['audio'], 'music.mp3', { type: 'audio/mpeg' });

    const plainTextResult = uploadFile(file);
    MockXMLHttpRequest.instances[0].respond(502, '上游服务暂时不可用');
    await expect(plainTextResult).rejects.toMatchObject({
      status: 502,
      code: undefined,
      message: '上游服务暂时不可用',
    });

    const htmlResult = uploadFile(file);
    MockXMLHttpRequest.instances[1].respond(
      503,
      '<html><body><h1>服务维护中</h1><p>请稍后重试</p><script>ignore()</script></body></html>',
    );
    await expect(htmlResult).rejects.toMatchObject({
      status: 503,
      code: undefined,
      message: '服务维护中请稍后重试',
    });
  });

  it('upload reuses 401 session cleanup and supports AbortSignal', async () => {
    const { uploadFile } = await import('../../src/api/files');
    const file = new File(['audio'], 'music.mp3', { type: 'audio/mpeg' });

    const unauthorized = uploadFile(file);
    MockXMLHttpRequest.instances[0].respond(401, '登录状态已失效');
    await expect(unauthorized).rejects.toEqual(
      expect.objectContaining({ status: 401, message: '登录状态已失效' }),
    );
    expect(localStorage.getItem('token')).toBeNull();

    const controller = new AbortController();
    const cancelled = uploadFile(file, undefined, controller.signal);
    controller.abort();
    await expect(cancelled).rejects.toEqual(
      expect.objectContaining({ name: 'AbortError', message: '上传已取消' }),
    );
  });
});
