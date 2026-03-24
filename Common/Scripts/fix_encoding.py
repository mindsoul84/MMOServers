"""
fix_encoding.py
===============
소스코드 파일을 UTF-8 BOM + CRLF 로 일괄 변환하는 스크립트.

[사용법 - Claude에게 요청할 때]
  소스코드 zip 파일과 이 스크립트(fix_encoding.py)를 함께 첨부하면
  Claude가 수정 후 자동으로 올바른 인코딩으로 변환하여 다운로드 제공합니다.

[직접 로컬에서 실행할 때]
  pip install chardet        (최초 1회)
  python fix_encoding.py <대상_폴더_또는_zip>

주의: chardet은 인코딩 감지에만 사용하며, UTF-8로 읽힐 경우 재인코딩하지 않습니다.
"""

import os
import sys
import zipfile
import shutil
import re

BOM = b'\xef\xbb\xbf'
TARGET_EXTENSIONS = {'.h', '.cpp', '.c', '.cc', '.cxx', '.bat', '.proto',
                     '.json', '.sql', '.txt', '.md', '.cs', '.py', '.java'}


def safe_decode(raw_bytes: bytes) -> tuple[str, str]:
    """
    바이트를 안전하게 문자열로 디코딩.
    전략: UTF-8 → CP949 → latin-1(무손실) 순서로 시도.
    chardet에 의존하지 않아 한글 깨짐 없음.
    """
    # BOM 제거
    if raw_bytes.startswith(BOM):
        raw_bytes = raw_bytes[3:]

    for enc in ('utf-8', 'cp949', 'euc-kr', 'latin-1'):
        try:
            return raw_bytes.decode(enc), enc
        except (UnicodeDecodeError, LookupError):
            continue
    # 최후 수단
    return raw_bytes.decode('utf-8', errors='replace'), 'utf-8(replace)'


def convert_file(path: str, extra_replacements: dict[str, str] | None = None) -> bool:
    """
    파일을 UTF-8 BOM + CRLF 로 변환.
    extra_replacements: {'찾을문자열': '바꿀문자열'} 형태의 추가 치환 규칙.
    변경이 있으면 True, 없으면 False 반환.
    """
    with open(path, 'rb') as f:
        raw = f.read()

    text, detected_enc = safe_decode(raw)

    # 추가 치환 적용 (예: IGNORE → IGNORED_ERROR)
    if extra_replacements:
        for old, new in extra_replacements.items():
            text = text.replace(old, new)

    # LF → CRLF (중복 방지)
    text_crlf = text.replace('\r\n', '\n').replace('\n', '\r\n')

    new_bytes = BOM + text_crlf.encode('utf-8')

    if new_bytes == raw:
        return False

    with open(path, 'wb') as f:
        f.write(new_bytes)
    return True


def process_directory(root_dir: str,
                      extra_replacements: dict[str, str] | None = None) -> list[str]:
    """폴더 내 모든 대상 파일을 변환하고 변환된 파일 목록 반환."""
    changed = []
    for dirpath, _, files in os.walk(root_dir):
        for fname in sorted(files):
            if os.path.splitext(fname)[1].lower() not in TARGET_EXTENSIONS:
                continue
            fpath = os.path.join(dirpath, fname)
            if convert_file(fpath, extra_replacements):
                changed.append(os.path.relpath(fpath, root_dir))
    return changed


def repack_zip(src_zip: str, dst_zip: str,
               extra_replacements: dict[str, str] | None = None) -> list[str]:
    """
    zip 압축 파일을 풀고, 변환 후, 새 zip으로 재패키징.
    """
    import tempfile
    with tempfile.TemporaryDirectory() as tmpdir:
        with zipfile.ZipFile(src_zip, 'r') as zf:
            zf.extractall(tmpdir)
        changed = process_directory(tmpdir, extra_replacements)
        if os.path.exists(dst_zip):
            os.remove(dst_zip)
        with zipfile.ZipFile(dst_zip, 'w', zipfile.ZIP_DEFLATED) as zf:
            for dirpath, _, files in os.walk(tmpdir):
                for fname in files:
                    fpath = os.path.join(dirpath, fname)
                    arcname = os.path.relpath(fpath, tmpdir)
                    zf.write(fpath, arcname)
    return changed


# ─────────────────────────────────────────────
# Claude가 사용하는 핵심 함수
# ─────────────────────────────────────────────
def run_for_claude(src_zip: str, dst_zip: str,
                   extra_replacements: dict[str, str] | None = None) -> None:
    """
    Claude가 파일 수정 후 호출하는 함수.
    src_zip  : 수정된 소스가 들어있는 zip 경로
    dst_zip  : 출력 zip 경로
    extra_replacements: 추가 문자열 치환 규칙 (선택)
    """
    print(f"[fix_encoding] 변환 시작: {src_zip} → {dst_zip}")
    changed = repack_zip(src_zip, dst_zip, extra_replacements)
    print(f"[fix_encoding] 완료: {len(changed)}개 파일 변환됨")
    for f in changed:
        print(f"  ✅ {f}")


# ─────────────────────────────────────────────
# 로컬 직접 실행
# ─────────────────────────────────────────────
if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("사용법: python fix_encoding.py <폴더_또는_.zip> [출력.zip]")
        sys.exit(1)

    target = sys.argv[1]

    if target.endswith('.zip'):
        out = sys.argv[2] if len(sys.argv) > 2 else target.replace('.zip', '_fixed.zip')
        changed = repack_zip(target, out)
        print(f"출력: {out}  ({len(changed)}개 파일 변환)")
    elif os.path.isdir(target):
        changed = process_directory(target)
        print(f"변환 완료: {len(changed)}개 파일")
    else:
        print(f"오류: '{target}' 을 찾을 수 없습니다.")
        sys.exit(1)

    for f in changed:
        print(f"  ✅ {f}")
