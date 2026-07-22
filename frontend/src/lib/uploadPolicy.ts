import type { UserRole } from '../types/api';

const MEBIBYTE = 1024 * 1024;

export const NORMAL_UPLOAD_LIMIT = 10 * MEBIBYTE;
export const VIP_UPLOAD_LIMIT = 100 * MEBIBYTE;

export const AUDIO_EXTENSIONS = [
  'mp3',
  'ogg',
  'wav',
  'flac',
  'aac',
  'm4a',
  'wma',
  'ape',
  'opus',
] as const;

export type AudioExtension = (typeof AUDIO_EXTENSIONS)[number];

export const AUDIO_ACCEPT = AUDIO_EXTENSIONS.map((extension) => `.${extension}`).join(',');

const MIME_TYPES: Record<AudioExtension, readonly string[]> = {
  mp3: ['audio/mpeg', 'audio/mp3'],
  ogg: ['audio/ogg', 'application/ogg'],
  wav: ['audio/wav', 'audio/x-wav', 'audio/wave'],
  flac: ['audio/flac', 'audio/x-flac'],
  aac: ['audio/aac', 'audio/x-aac'],
  m4a: ['audio/mp4', 'audio/x-m4a', 'audio/m4a'],
  wma: ['audio/x-ms-wma', 'audio/wma'],
  ape: ['audio/x-monkeys-audio', 'audio/x-ape', 'audio/ape'],
  opus: ['audio/opus', 'audio/ogg'],
};

export interface UploadValidationResult {
  valid: boolean;
  error?: string;
}

export function getAudioExtension(fileName: string): AudioExtension | null {
  const dotIndex = fileName.lastIndexOf('.');
  if (dotIndex < 0 || dotIndex === fileName.length - 1) return null;

  const extension = fileName.slice(dotIndex + 1).toLowerCase();
  return AUDIO_EXTENSIONS.includes(extension as AudioExtension)
    ? extension as AudioExtension
    : null;
}

export function getUploadLimit(role: UserRole | null | undefined): number {
  return role === 'VIP' ? VIP_UPLOAD_LIMIT : NORMAL_UPLOAD_LIMIT;
}

export function getContentType(file: File): string {
  if (file.type.trim()) return file.type.toLowerCase();

  const extension = getAudioExtension(file.name);
  return extension ? MIME_TYPES[extension][0] : 'application/octet-stream';
}

export function validateUploadFile(
  file: File,
  role: UserRole | null | undefined,
): UploadValidationResult {
  const extension = getAudioExtension(file.name);
  if (!extension) {
    return {
      valid: false,
      error: `不支持该文件类型，仅允许：${AUDIO_EXTENSIONS.join('、')}`,
    };
  }

  if (file.size === 0) {
    return { valid: false, error: '文件内容为空，无法上传' };
  }

  const contentType = file.type.trim().toLowerCase();
  if (
    contentType
    && contentType !== 'application/octet-stream'
    && !MIME_TYPES[extension].includes(contentType)
  ) {
    return {
      valid: false,
      error: `文件扩展名 .${extension} 与浏览器识别的类型 ${contentType} 不一致`,
    };
  }

  const limit = getUploadLimit(role);
  if (file.size > limit) {
    return {
      valid: false,
      error: `文件超过当前账号 ${formatFileSize(limit)} 的上传限制`,
    };
  }

  return { valid: true };
}

export function formatFileSize(size: number): string {
  if (size < 1024) return `${size} B`;
  if (size < MEBIBYTE) return `${(size / 1024).toFixed(1)} KiB`;
  return `${(size / MEBIBYTE).toFixed(size % MEBIBYTE === 0 ? 0 : 1)} MiB`;
}
