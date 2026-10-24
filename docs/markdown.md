# Markdown and formulas

[English](markdown.md) | [简体中文](markdown.zh-CN.md)

Place UTF-8 `.md` or `.markdown` files on SD or upload through A. Extensions are case-insensitive; the source limit is 1 MiB. Open and turn pages like other books, without networking or formula images.

## Prose

Supported constructs include headings, paragraphs, ordered/unordered/task lists, quotes, emphasis, bold, strikethrough, rules, inline code and fenced code. Links show text and an underline without opening a browser. Images retain alt text without loading pixels. Headings use weight and paragraph spacing, without a separate size per level.

English emphasis uses italic where available; other glyphs use available styles. Code uses actual native 24 px glyph advances, without stretching every character to at least 24 px; the font is not monospaced. Source spaces, indentation and newlines remain, with no hyphenation or justification. Markdown tables remain complete preformatted source rather than grid rendering. HTML is not executed and external resources are not loaded.

## Formula syntax

Use `$...$` or `\(...\)` inline, and `$$...$$` or `\[...\]` for display math. Put display delimiters on separate lines where possible. Delimiters inside code fences remain literal text.

```markdown
Pythagoras: $a^2+b^2=c^2$.

$$
x=\frac{-b\pm\sqrt{b^2-4ac}}{2a}
$$

$$
\begin{bmatrix}
a & b \\
c & d
\end{bmatrix}
$$
```

The device lays out formula width, baseline, ascent and descent. This is a bounded TeX subset, not a complete TeX engine.

| Category | Examples |
| --- | --- |
| Scripts and derivatives | `x_i^2`, `f'`, `f''` |
| Fractions and binomials | `\frac`, `\dfrac`, `\tfrac`, `\binom`, `\dbinom`, `\tbinom` |
| Roots and delimiters | `\sqrt{x}`, `\sqrt[3]{x}`, `\left(...\right)`; delimiter height is limited by built-in variants |
| Operators and functions | `\sum`, `\prod`, `\int`, `\oint`, `\sin`, `\cos`, `\log`, `\exp`, `\lim`, `\det`; `\limits` / `\nolimits` |
| Letters and symbols | Greek, common relations, sets, arrows and logic, including `\alpha`, `\leq`, `\in`, `\to`, `\forall` |
| Fonts and accents | `\mathrm`, `\mathit`, `\mathbf`, `\mathbb`, `\mathcal`, `\bar`, `\overline`, `\underline`, `\hat`, `\vec`; subject to subset glyph coverage |
| Short text | `\text{...}`, `\operatorname{...}`; math-font coverage only, without nested formatting or arbitrary Chinese paragraphs |
| Matrices and alignment | `matrix`, `pmatrix`, `bmatrix`, `Bmatrix`, `vmatrix`, `Vmatrix`, `cases`, `aligned`; `&` separates columns, `\\` separates rows |
| Spacing | `\,`, `\:`, `\;`, `\!`, `\quad`, `\qquad`, backslash-space |

Unlisted commands are not guaranteed. Custom macros, packages, full documents, automatic equation numbering and general TeX environments are not implemented. `cases` requires two columns; ordinary matrices require equal column counts; `aligned` allows unequal counts. Multiline math remains one object and is not split across pages.

## Fallback and bounds

Unsupported syntax, missing glyphs or exceeded size/budget retain the entire original formula, including delimiters and newlines, as text. No partial formula is silently accepted. A failed whole-document parse falls back to source reading; empty files and invalid UTF-8 are rejected.

Each formula allows at most 4096 bytes, 256 parse nodes, 1024 drawing commands, 16 parsing levels and an 8×8 matrix. It must also fit the available page. These are ceilings, not a promise that every combination fits; split long expressions into separate display formulas.

The math font is an OFL subset of STIX Two Math, rasterized at native 28/20/14 px. Scripts and nesting switch native sizes rather than scaling bitmaps at runtime. See [development](development.md) for font, variant and MATH-constant generation.

## Source and progress

SD source files are not rewritten. Progress uses original UTF-8 byte offsets, including a leading BOM. Generated list markers, decoded entities and formula objects map back to original positions. Caches store source and page starts, separating TXT/EPUB versus Markdown and P/S settings. The current layout family is `0x2026a000`. After a layout change, old caches may supply read-only source-anchor migration only after full validation; old page starts cannot remain a current hit.

Reflow can change page count/numbers while restoring the corresponding source position. Temporary whole-document fallback pagination does not replace a valid Markdown cache. Editing content or changing paths changes book identity; arbitrary edits do not guarantee automatic progress migration.
