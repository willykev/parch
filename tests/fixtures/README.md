# Test fixtures

Conformance examples the Markdown engine is checked against. They are test
data, not part of the application, and keep their own licences.

| File | What | Source | Licence |
| --- | --- | --- | --- |
| `heading-slugs.json` | Heading texts and the anchor ids generated for them | Written for parch | As parch |
| `themes/*/colors.toml` | A dark and a light palette in Omarchy's theme format, of low contrast, for checks that run where no Omarchy theme is installed | Written for parch | As parch |
| `commonmark-0.31.2.json` | The 652 examples of the CommonMark specification, version 0.31.2, unchanged | <https://spec.commonmark.org/0.31.2/spec.json> | [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/), © John MacFarlane |
| `gfm-extensions.json` | The 23 examples of the tables, task list items, strikethrough and autolinks extensions of the GitHub Flavored Markdown specification, version 0.29-gfm | `test/spec.txt` of <https://github.com/github/cmark-gfm> | [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/), © GitHub, Inc., based on the CommonMark specification |
| `math/katex-0.16.json` | One TeX expression for each command KaTeX 0.16 lists as supported, and whether the pinned math helper draws it | Written for parch; only the command names and their groups follow KaTeX's list of supported functions | As parch |

`heading-slugs.json` is parch's own: the heading texts and the anchor ids
they must get, in the engine, the preview and every export alike.

`math/katex-0.16.json` holds, for each command, its group, the command, an
expression that uses it, `drawn` and, for one that does not draw, the
helper's `diagnostic`. `katexCorpusDrawsAsRecorded` draws every expression
with the helper in `build/helpers` and fails on any change from the record;
`PARCH_RECORD_MATH_CORPUS=1 bin/test katexCorpusDrawsAsRecorded` writes what
the helper does now as the new record (and its version as `helper`), for a
helper update. The user guide's math section names what does not draw.

`gfm-extensions.json` holds the examples marked `table`, `disabled`
(task list items), `strikethrough` and `autolink` in the specification's
source, in the JSON shape of the CommonMark file: `markdown`, `html`,
`example` (the example's number in the GFM specification), `start_line`,
`end_line` and `section`. The arrows that stand for tabs in the
specification's source are tabs here.
