import { describe, expect, it } from 'vitest';
import {
  AUDIO_EXTENSIONS,
  NORMAL_UPLOAD_LIMIT,
  VIP_UPLOAD_LIMIT,
  validateUploadFile,
} from '../../src/lib/uploadPolicy';

function file(name: string, size: number, type = ''): File {
  return { name, size, type } as File;
}

describe('上传文件前置校验', () => {
  it('九种音频扩展名均支持且大小写不敏感', () => {
    for (const extension of AUDIO_EXTENSIONS) {
      expect(validateUploadFile(file(`TRACK.${extension.toUpperCase()}`, 1), 'NORMAL')).toEqual({ valid: true });
    }
  });

  it('拒绝未知扩展名、空文件和明显 MIME 冲突', () => {
    expect(validateUploadFile(file('notes.txt', 10, 'text/plain'), 'NORMAL')).toEqual(
      expect.objectContaining({ valid: false, error: expect.stringContaining('不支持') }),
    );
    expect(validateUploadFile(file('empty.mp3', 0, 'audio/mpeg'), 'NORMAL')).toEqual(
      expect.objectContaining({ valid: false, error: expect.stringContaining('为空') }),
    );
    expect(validateUploadFile(file('cover.mp3', 10, 'image/png'), 'NORMAL')).toEqual(
      expect.objectContaining({ valid: false, error: expect.stringContaining('不一致') }),
    );
  });

  it('通用二进制 MIME 不误伤合法扩展名', () => {
    expect(validateUploadFile(file('track.flac', 10, 'application/octet-stream'), 'NORMAL')).toEqual({ valid: true });
  });

  it('NORMAL 和 VIP 使用各自单文件大小上限', () => {
    expect(validateUploadFile(file('normal.mp3', NORMAL_UPLOAD_LIMIT), 'NORMAL')).toEqual({ valid: true });
    expect(validateUploadFile(file('normal.mp3', NORMAL_UPLOAD_LIMIT + 1), 'NORMAL')).toEqual(
      expect.objectContaining({ valid: false, error: expect.stringContaining('10 MiB') }),
    );
    expect(validateUploadFile(file('vip.mp3', VIP_UPLOAD_LIMIT), 'VIP')).toEqual({ valid: true });
    expect(validateUploadFile(file('vip.mp3', VIP_UPLOAD_LIMIT + 1), 'VIP')).toEqual(
      expect.objectContaining({ valid: false, error: expect.stringContaining('100 MiB') }),
    );
  });
});
