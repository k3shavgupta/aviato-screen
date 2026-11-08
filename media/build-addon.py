#!/usr/bin/env python3
"""Build an installable Kodi ZIP; no downloads or dependencies."""
from pathlib import Path
import zipfile
root = Path(__file__).resolve().parent
output = root / 'dist/screensaver.aviato-1.0.0.zip'
output.parent.mkdir(exist_ok=True)
with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
    for path in sorted((root / 'screensaver.aviato').rglob('*')):
        if path.is_file() and '__pycache__' not in path.parts:
            archive.write(path, path.relative_to(root))
print(output)
