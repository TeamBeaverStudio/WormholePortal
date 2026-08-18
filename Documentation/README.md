# Wormhole Portal Documentation

The official Wormhole Portal documentation site, built with
[Zensical](https://zensical.org/) and prepared for GitHub Pages. English is
published at the site root, and Korean is published under `/ko/`.

## One-click preview on Windows

Double-click `PreviewDocs.cmd`.

The preview server starts and the documentation opens in your default browser.
Keep the command window open while previewing, and close it when finished.
The first launch also prepares the local Zensical environment when needed.

## Run locally on Windows

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\zensical.exe serve
```

Open <http://127.0.0.1:8000/> in a browser.

## Build the static site

```powershell
.\.venv\Scripts\zensical.exe build --config-file zensical.toml --clean --strict
.\.venv\Scripts\zensical.exe build --config-file zensical.ko.toml --clean --strict
New-Item -ItemType Directory -Force -Path .\site\ko
Copy-Item -Recurse -Force .\site-ko\* .\site\ko\
```

The merged site is written to `site/`. English is available at `/`, and Korean
is available at `/ko/`.

## Project structure

- `docs/` contains the English pages and shared assets.
- `docs-ko/` contains the Korean pages.
- `zensical.toml` and `zensical.ko.toml` configure each language build.
- `.github/workflows/docs.yml` builds, merges, and publishes both languages.
