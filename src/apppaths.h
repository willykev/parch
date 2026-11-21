#pragma once

#include <QString>

// Where parch keeps its own files. Every location is injectable, so that
// tests run beneath a temporary folder and never touch the real XDG folders.
// The runtime fallback is created privately when the session provides none.
struct AppPaths {
    QString data;
    QString state;
    QString config;
    QString runtime;
    bool singleInstance = true;

    // Whether the id is a document id as parch makes them: a UUID in lower
    // case without braces, and never empty. Ids name folders and files
    // beneath these locations, so nothing else is accepted.
    static bool isDocumentId(const QString &id);

    // The XDG data, state and config folders, each with a parch subfolder, and
    // the session's runtime folder.
    static AppPaths standard();
    // Every location beneath one folder.
    static AppPaths under(const QString &root);

    QString socketPath() const;
    bool socketPathFits() const;

    // Unnamed drafts, one folder per document id.
    QString drafts() const;
    QString recovery() const;
    QString history() const;
    QString sessionFile() const;
    // Maps canonical file paths to document ids.
    QString registryFile() const;
    // The Markdown settings kept for documents and library folders.
    QString documentSettingsFile() const;
    QString settingsFile() const;
    QString recentFile() const;
    // Where the writer was in each file, beside the recent files.
    QString positionsFile() const;
    // The words the writer added to the spelling dictionary, one per line.
    QString personalDictionaryFile() const;
};
