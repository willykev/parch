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
- Preferences for the writing font, text size, line height, line length,
  heading syntax and toolbar, applied at once in every window.
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
