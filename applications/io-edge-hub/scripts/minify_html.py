#!/usr/bin/env python3
"""Minimal HTML minifier: strip comments, collapse whitespace."""
import re, sys

def minify(html):
    html = re.sub(r'<!--.*?-->', '', html, flags=re.DOTALL)
    html = re.sub(r'/\*.*?\*/', '', html, flags=re.DOTALL)
    html = re.sub(r'^\s*//[^\n]*', '', html, flags=re.MULTILINE)
    html = re.sub(r'\n\s*\n', '\n', html)
    html = re.sub(r'[ \t]+', ' ', html)
    html = re.sub(r'\n', '', html)
    html = re.sub(r'>\s+<', '><', html)
    return html.strip()

if __name__ == '__main__':
    with open(sys.argv[1], 'r', encoding='utf-8') as f:
        sys.stdout.write(minify(f.read()))
