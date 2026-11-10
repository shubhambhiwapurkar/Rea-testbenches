"""Writes a publishable single page from Web/index.html.

Hosts that wrap pages in their own document skeleton (claude.ai artifacts) want the page content
without <html>, <head> or <body> tags. Everything else (scripts, styles, assets) is published
alongside from Web/ at the same relative paths.

    python3 Tools/web/make_artifact_page.py /path/to/output.html
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]


def main() -> None:
    out = pathlib.Path(sys.argv[1])
    html = (ROOT / "Web/index.html").read_text()
    body = re.search(r"<body>(.*)</body>", html, re.S).group(1).strip()
    page = "\n".join([
        "<title>Summit Everest</title>",
        '<link rel="stylesheet" href="css/style.css">',
        body,
        "",
    ])
    out.write_text(page)
    print(f"wrote {out} ({len(page)} bytes)")


if __name__ == "__main__":
    main()
