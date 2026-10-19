#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
md2html.py -- a small Markdown -> HTML converter for the C* manual.

supports:  # headings, ``` code fences, | tables |, - and 1. lists,
           > quotes, --- rules, **bold**, *italic*, `code`, [links](url)

usage:  python md2html.py [input.md] [output.html]
        (defaults:  docs/CStar手册.md  ->  docs/manual.html)
"""
import html
import os
import re
import sys


def inline(text):
    s = html.escape(text, quote=False)
    # code spans first, stashed so other rules do not touch them
    codes = []

    def stash(m):
        codes.append(m.group(1)[1:-1])
        return "\x00%d\x01" % (len(codes) - 1)

    s = re.sub(r"(`[^`]+`)", stash, s)
    s = re.sub(r"!\[([^\]]*)\]\(([^)\s]+)\)",
               lambda m: '<img src="%s" alt="%s">' % (m.group(2), m.group(1)), s)
    s = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)",
               lambda m: '<a href="%s">%s</a>' % (m.group(2), m.group(1)), s)
    s = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", s)
    s = re.sub(r"(?<!\*)\*([^*\n]+)\*(?!\*)", r"<em>\1</em>", s)
    s = re.sub(r"\x00(\d+)\x01", lambda m: "<code>%s</code>" % codes[int(m.group(1))], s)
    return s


def is_table_sep(line):
    return bool(re.match(r"^\s*\|?[\s:|-]+\|[\s:|-]*$", line)) and "-" in line


def table_cells(line):
    line = line.strip()
    if line.startswith("|"):
        line = line[1:]
    if line.endswith("|"):
        line = line[:-1]
    return [c.strip() for c in line.split("|")]


def convert(md):
    lines = md.splitlines()
    out = []
    i = 0
    n = len(lines)
    para = []

    def flush_para():
        if para:
            out.append("<p>" + inline("\n".join(para)) + "</p>")
            del para[:]

    while i < n:
        line = lines[i]
        stripped = line.strip()

        # fenced code block
        if stripped.startswith("```"):
            flush_para()
            lang = stripped[3:].strip()
            i += 1
            code = []
            while i < n and not lines[i].strip().startswith("```"):
                code.append(lines[i])
                i += 1
            i += 1  # closing fence
            cls = ' class="lang-%s"' % lang if lang else ""
            out.append("<pre><code%s>%s</code></pre>"
                       % (cls, html.escape("\n".join(code), quote=False)))
            continue

        # blank line
        if not stripped:
            flush_para()
            i += 1
            continue

        # heading
        m = re.match(r"^(#{1,6})\s+(.*)$", stripped)
        if m:
            flush_para()
            lvl = len(m.group(1))
            out.append("<h%d>%s</h%d>" % (lvl, inline(m.group(2)), lvl))
            i += 1
            continue

        # horizontal rule
        if re.match(r"^(-{3,}|\*{3,})$", stripped):
            flush_para()
            out.append("<hr>")
            i += 1
            continue

        # table
        if stripped.startswith("|") and i + 1 < n and is_table_sep(lines[i + 1]):
            flush_para()
            header = table_cells(stripped)
            i += 2
            rows = []
            while i < n and lines[i].strip().startswith("|"):
                rows.append(table_cells(lines[i].strip()))
                i += 1
            out.append("<table>")
            out.append("<thead><tr>" +
                       "".join("<th>%s</th>" % inline(c) for c in header) +
                       "</tr></thead>")
            out.append("<tbody>")
            for r in rows:
                out.append("<tr>" +
                           "".join("<td>%s</td>" % inline(c) for c in r) +
                           "</tr>")
            out.append("</tbody></table>")
            continue

        # block quote
        if stripped.startswith(">"):
            flush_para()
            quote = []
            while i < n and lines[i].strip().startswith(">"):
                quote.append(lines[i].strip().lstrip(">").strip())
                i += 1
            out.append("<blockquote><p>" + inline("\n".join(quote)) + "</p></blockquote>")
            continue

        # lists
        m = re.match(r"^(\s*)([-*]|\d+\.)\s+(.*)$", line)
        if m:
            flush_para()
            ordered = m.group(2)[0].isdigit()
            tag = "ol" if ordered else "ul"
            items = []
            while i < n:
                mm = re.match(r"^(\s*)([-*]|\d+\.)\s+(.*)$", lines[i])
                if not mm or mm.group(2)[0].isdigit() != ordered:
                    break
                items.append("<li>" + inline(mm.group(3)) + "</li>")
                i += 1
            out.append("<%s>" % tag + "".join(items) + "</%s>" % tag)
            continue

        # plain paragraph line
        para.append(stripped)
        i += 1

    flush_para()
    return "\n".join(out)


TEMPLATE = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<title>{title}</title>
<style>
  body {{ font-family: "Segoe UI", "Microsoft YaHei", "PingFang SC", sans-serif;
         line-height: 1.7; color: #222; background: #fafafa;
         max-width: 920px; margin: 0 auto; padding: 24px 20px 80px; }}
  h1 {{ border-bottom: 3px solid #2b6cb0; padding-bottom: 8px; }}
  h2 {{ border-bottom: 1px solid #ddd; padding-bottom: 6px; margin-top: 2em; }}
  a {{ color: #2b6cb0; }}
  code {{ background: #eef1f5; padding: 1px 6px; border-radius: 4px;
          font-family: Consolas, "Courier New", monospace; font-size: 0.95em; }}
  pre {{ background: #1e2430; color: #e6e6e6; padding: 14px 16px;
         border-radius: 8px; overflow-x: auto; line-height: 1.5; }}
  pre code {{ background: none; color: inherit; padding: 0; }}
  img {{ max-width: 100%; border: 1px solid #ccd; border-radius: 6px;
         margin: 10px 0; }}
  table {{ border-collapse: collapse; margin: 1em 0; width: 100%; }}
  th, td {{ border: 1px solid #ccd; padding: 7px 10px; text-align: left; }}
  th {{ background: #eef1f5; }}
  blockquote {{ border-left: 4px solid #2b6cb0; margin: 1em 0;
                padding: 4px 16px; color: #444; background: #f0f4f8; }}
  hr {{ border: none; border-top: 1px solid #ddd; margin: 2.5em 0; }}
  li {{ margin: 3px 0; }}
</style>
</head>
<body>
{body}
<hr>
<p style="color:#888;font-size:0.9em;">由 docs/md2html.py 从 {src} 生成</p>
</body>
</html>
"""


def title_of(md, fallback):
    for line in md.splitlines():
        m = re.match(r"^#\s+(.*)$", line.strip())
        if m:
            return m.group(1)
    return fallback


def convert_file(src, dst):
    with open(src, "r", encoding="utf-8") as f:
        md = f.read()
    body = convert(md)
    name = os.path.basename(src)
    with open(dst, "w", encoding="utf-8") as f:
        f.write(TEMPLATE.format(body=body, src=name,
                                title=title_of(md, name)))
    print("wrote %s" % dst)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    if len(sys.argv) > 2:
        convert_file(sys.argv[1], sys.argv[2])
        return
    # every docs/*.md becomes docs/<name>.html; the manual keeps its old name
    names = sorted(f for f in os.listdir(here) if f.endswith(".md"))
    for md in names:
        html = "manual.html" if md == "CStar手册.md" else md[:-3] + ".html"
        convert_file(os.path.join(here, md), os.path.join(here, html))


if __name__ == "__main__":
    main()
