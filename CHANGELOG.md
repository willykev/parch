# Changelog

## 0.1.0 — unreleased

The first version: a native Markdown writing app for Omarchy.

### Writing

- Markdown as plain text, with its punctuation in view, in a window that
  follows the Omarchy theme and font, light or dark.
- Tabs, separate windows, and several views of one document.
- Drafts that are kept from the first keystroke and named when saved.
- Each file's line endings and byte-order mark are kept.
- Rich text pastes as clean Markdown; `Ctrl+Shift+V` pastes plain text.
- Pasted images and dropped files go into an `assets` folder beside the
  document; a dropped Markdown document can be linked or included.
- Check links lists the links and images whose files are missing.
- Formatting commands that write and toggle Markdown, each one undo step, from
  the menu, a formatting toolbar, a command palette and their keys.
- Lists that continue on Enter and indent with Tab, table cells reached with
  Tab, table formatting, and Shift+Enter for a hard line break.
- Keys you can change, with refusals for keys that type and swapping for keys
  taken.
- English spell checking as you write, with wavy underlines, suggestions, a
  personal dictionary and Ignore, from a right click or the `Menu` key.
- Focus mode, which dims all but the sentence or paragraph being written, and
  distraction-free writing, which shows the text alone.
- Preferences for the writing font, text size, line height, line length,
  heading syntax, toolbar, spelling and focus mode, applied at once in every
  window.
- A word count and reading time for the document or the selection.

### Keeping text safe

- Saves as you pause, replacing the file in one step and keeping its
  permissions, group, extended attributes and access lists.
- Crash recovery for text that has not reached its file yet.
- Version history with the reason each version was kept, named versions, and
  restore with undo.
- Conflicts when a file changes on disk under unsaved text, with a side by
  side review and Reload, Keep both, Overwrite or Save as.
- A file replaced by a link to another file is never read or written through
  that link unless you choose to follow it.

### Finding documents

- Find and replace in a document, with case, whole-word and regular
  expression matching; search across every library folder; go to a heading.
- Make, rename, move and trash files from the library, with links to them
  updated, and an outline of the document in front.
- Each document reopens where you were in it.
- A library of folders, drafts and recent files.
- Find a document by name across every library folder.
- Open files from the terminal, in the running window when there is one.
- The session reopens your windows and tabs where you left them.

### From the terminal

- `parch check` reports what is wrong in documents and the files they
  include, with file, line and column: includes, references, footnotes,
  citations and variables that lead nowhere, missing images, links to files
  or headings that are not there, and more.
- `parch render` writes a document's HTML, plain text, outline or semantic
  tree, and `parch stats` counts its words, characters, reading time and
  headings.
- `parch export` writes a document as one Markdown file with its includes in
  place, or as a standalone HTML page, never over the document and never
  over another file unless asked. PDF, DOCX and EPUB are not available from
  the terminal yet.
- Every command needs no display, takes `--json`, and reads the Writer
  profile unless `--profile`, `--extension` or `--use-saved-settings` say
  otherwise.
- `parch skill install` installs a skill that teaches coding agents to use
  parch.
