# Test fixtures

Conformance examples the Markdown engine is checked against. They are test
data, not part of the application, and keep their own licences.

| File | What | Source | Licence |
| --- | --- | --- | --- |
| `heading-slugs.json` | Heading texts and the anchor ids generated for them | Written for parch | As parch |
| `themes/*/colors.toml` | A dark and a light palette in Omarchy's theme format, of low contrast, for checks that run where no Omarchy theme is installed | Written for parch | As parch |
| `commonmark-0.31.2.json` | The 652 examples of the CommonMark specification, version 0.31.2, unchanged | <https://spec.commonmark.org/0.31.2/spec.json> | [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/), © John MacFarlane |
| `gfm-extensions.json` | The 23 examples of the tables, task list items, strikethrough and autolinks extensions of the GitHub Flavored Markdown specification, version 0.29-gfm | `test/spec.txt` of <https://github.com/github/cmark-gfm> | [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/), © GitHub, Inc., based on the CommonMark specification |

`heading-slugs.json` is parch's own: the heading texts and the anchor ids
they must get, in the engine, the preview and every export alike.

`gfm-extensions.json` holds the examples marked `table`, `disabled`
(task list items), `strikethrough` and `autolink` in the specification's
source, in the JSON shape of the CommonMark file: `markdown`, `html`,
`example` (the example's number in the GFM specification), `start_line`,
`end_line` and `section`. The arrows that stand for tabs in the
specification's source are tabs here.
