#pragma once

#include "mdtree.h"
#include "sourcetypes.h"

#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QThreadPool>
#include <memory>
#include <vector>

class Hunspell;

// The prose of a Markdown text, which spelling checks: the Markdown
// engine's prose ranges (md::Document::proseRanges), in order, those that
// touch joined into one. Code, math, URLs, comments, front matter, raw
// HTML, link destinations, labels and keys, and Markdown punctuation are
// not prose.
std::vector<md::Range> spellingProse(const QString &text);

// The words in [from, to) of `text` within the ranges of prose (in order and
// apart), as Unicode segments them: apostrophes inside a word, accents and
// other marks belong to it, and a word joined to the next by a hyphen is one
// word with it. What lies between two ranges parts words, and so do
// underscores. A word with a digit in it, one of a single letter, one with
// no Latin letter, one with a dot or colon inside, as a domain or "e.g",
// and one in a URL written as text (`scheme://…`, `mailto:…`), is no word
// to check.
QList<SourceRange> spellingWords(QStringView text, int from, int to, const std::vector<md::Range> &prose);

// The checking itself, shared by every view and their workers: Hunspell with
// one dictionary, the personal dictionary and the words ignored for the
// session, behind one lock, with the answers kept for each word asked.
class SpellEngine {
public:
    SpellEngine();
    ~SpellEngine();
    SpellEngine(const SpellEngine &) = delete;
    SpellEngine &operator=(const SpellEngine &) = delete;

    // Reads a Hunspell dictionary; false, with the reason, when it cannot.
    bool load(const QString &affix, const QString &dictionary, QString *error);
    bool loaded() const;
    // Whether the word is spelled right, by the personal dictionary, the
    // words ignored, or the dictionary. Any thread.
    bool check(const QString &word);
    // Up to `count` spellings the dictionary suggests instead.
    QStringList suggest(const QString &word, int count);
    void addPersonal(const QString &word);
    void ignore(const QString &word);
    // Taken already by the personal dictionary or the words ignored.
    bool accepted(const QString &word) const;

private:
    // The word as the dictionary spells it: composed, with a typographic
    // apostrophe as the plain one. Empty when the dictionary's encoding cannot hold it.
    QByteArray encode(const QString &word) const;
    QString decode(const std::string &word) const;

    mutable QMutex m_mutex;
    std::unique_ptr<Hunspell> m_hunspell;
    bool m_latin1 = false;
    QSet<QString> m_personal, m_ignored;
    QHash<QString, bool> m_answers;
};

// English spelling for the whole application: the en_US Hunspell dictionary,
// found in the dictionary folders and read off the UI thread, the personal
// dictionary kept in a file of its own, one word per line, and the words
// ignored until parch ends. A word added or ignored applies at once in every
// view. Whether spelling is wanted is the window's settings' to say; it is
// checked while it is wanted and a dictionary is read.
class SpellChecker : public QObject {
    Q_OBJECT
    // The dictionary's files are there, which is known as soon as load() is called.
    Q_PROPERTY(bool dictionaryFound READ dictionaryFound NOTIFY availabilityChanged)
    // The dictionary is read and can be asked.
    Q_PROPERTY(bool available READ available NOTIFY availabilityChanged)
    // Looking for and reading the dictionary is over, whatever it found.
    Q_PROPERTY(bool settled READ settled NOTIFY availabilityChanged)
    Q_PROPERTY(bool wanted READ wanted NOTIFY wantedChanged)
    // Wanted and available: the views check their text.
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    // Where the dictionary was looked for, for saying where it is missing.
    Q_PROPERTY(QStringList dictionaryFolders READ dictionaryFolders CONSTANT)

public:
    static constexpr int MaxSuggestions = 5;
    // The dictionary's name, for its files <Language>.aff and <Language>.dic.
    static constexpr const char *Language = "en_US";

    explicit SpellChecker(const QString &personalFile, QObject *parent = nullptr);
    ~SpellChecker() override;

    // Where Hunspell dictionaries are installed: the person's own data
    // folder, and those of the system, as XDG names them.
    static QStringList systemDictionaryFolders();
    // Where to look instead, before load().
    void setDictionaryFolders(const QStringList &folders);
    QStringList dictionaryFolders() const { return m_folders; }
    // Reads the personal dictionary, then looks for the dictionary and reads
    // it on a worker; `availabilityChanged` says when it is done.
    void load();

    bool dictionaryFound() const { return m_found; }
    bool available() const { return m_available; }
    bool settled() const { return m_settled; }
    bool wanted() const { return m_wanted; }
    void setWanted(bool wanted);
    bool active() const { return m_wanted && m_available; }
    QString personalFile() const { return m_personalFile; }

    std::shared_ptr<SpellEngine> engine() const { return m_engine; }
    // The worker the views check their text on, one at a time.
    QThreadPool *pool() const { return m_pool.get(); }

    Q_INVOKABLE QStringList suggestions(const QString &word) const;
    // Keeps the word in the personal dictionary; false, with a problem said,
    // when the file cannot be written, though it applies for the session.
    Q_INVOKABLE bool addToDictionary(const QString &word);
    Q_INVOKABLE void ignore(const QString &word);
    // Taken already by the personal dictionary or the words ignored.
    bool accepts(const QString &word) const { return m_engine->accepted(word); }
    QStringList personalWords() const { return m_personalWords; }

signals:
    void availabilityChanged();
    void wantedChanged();
    void activeChanged();
    // The word is now taken everywhere.
    void wordAccepted(const QString &word);
    // The personal dictionary could not be read or written.
    void problem(const QString &problem, const QString &detail);

private:
    void finishLoading(bool available, const QString &error);

    QString m_personalFile;
    QStringList m_folders;
    QStringList m_personalWords;
    std::shared_ptr<SpellEngine> m_engine;
    std::unique_ptr<QThreadPool> m_pool;
    bool m_found = false;
    bool m_available = false;
    bool m_settled = false;
    bool m_wanted = false;
    bool m_loading = false;
    bool m_personalUnreadable = false;
};
