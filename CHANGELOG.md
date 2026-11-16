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

- A library of folders, drafts and recent files.
- Find a document by name across every library folder.
- Open files from the terminal, in the running window when there is one.
- The session reopens your windows and tabs where you left them.
