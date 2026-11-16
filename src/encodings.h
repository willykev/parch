#pragma once

#include "diskfile.h"
// The document is a property QML reads, so its type must be complete here.
#include "document.h"

#include <QByteArray>
#include <QFutureWatcher>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <optional>

// Reading a file that is not UTF-8 in another encoding, as the writer
// chooses one: what can be chosen, and the text a choice gives.
namespace Encodings {

// An encoding by the name the converter knows it, and as it is shown.
struct Encoding {
    QString name;
    QString label;
};

// The encodings offered: the common ones first, then every other the
// system's converter knows, by name. Windows-1252, Latin-1 and UTF-16 are
// always there, whatever the converter knows.
QList<Encoding> offered();

struct Decoded {
    QString text;
    // The line ending most breaks use, as a file read as UTF-8 has it; no
    // byte-order mark, since the text is written as UTF-8 without one.
    TextConvention convention;
    // Whether the encoding is one this system can read.
    bool known = false;
    // How many characters the encoding could not read, each shown as U+FFFD.
    int failures = 0;
};

// The bytes read in the named encoding; a byte-order mark at the start is
// no part of the text.
Decoded decode(const QByteArray &bytes, const QString &name);

// How an encoding is shown, as offered() names it; its own name otherwise.
QString labelOf(const QString &name);

// The encoding a file that is not UTF-8 was most likely written in: as its
// byte-order mark says, UTF-16 when every other byte of its start is zero,
// and otherwise Windows-1252.
QString likely(const QByteArray &bytes);

} // namespace Encodings

// What the Reopen as… dialog works with, for a document open read-only
// because its file is not UTF-8: the file's bytes, read off the UI thread
// as the document is given, and the text the chosen encoding makes of them,
// decoded off the UI thread too, with a preview of its start. The likely
// encoding is chosen first. A result that a later choice overtook is dropped.
class EncodingChoice : public QObject {
    Q_OBJECT
    Q_PROPERTY(Document *document READ document WRITE setDocument NOTIFY documentChanged)
    // The encodings, as shown and by name, in the same order.
    Q_PROPERTY(QStringList labels READ labels CONSTANT)
    Q_PROPERTY(QStringList names READ names CONSTANT)
    Q_PROPERTY(int chosen READ chosen WRITE setChosen NOTIFY changed)
    Q_PROPERTY(QString name READ name NOTIFY changed)
    // The file is read and the chosen encoding's text is ready.
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    // The start of the text, at most PreviewLength characters.
    Q_PROPERTY(QString preview READ preview NOTIFY changed)
    Q_PROPERTY(int failures READ failures NOTIFY changed)
    // Why the file could not be read; empty otherwise.
    Q_PROPERTY(QString error READ error NOTIFY changed)

public:
    static constexpr int PreviewLength = 20000;

    explicit EncodingChoice(QObject *parent = nullptr);
    ~EncodingChoice() override;

    Document *document() const;
    void setDocument(Document *document);
    QStringList labels() const;
    QStringList names() const;
    int chosen() const;
    void setChosen(int index);
    QString name() const;
    bool ready() const;
    QString preview() const;
    int failures() const;
    QString error() const;
    // The whole text the chosen encoding makes of the file, and the version
    // of the file it was read from.
    Encodings::Decoded decoded() const;
    DiskVersion version() const;

signals:
    void documentChanged();
    void changed();

private:
    struct Read {
        QByteArray bytes;
        bool ok = false;
    };
    void decodeChosen();

    QPointer<Document> m_document;
    QList<Encodings::Encoding> m_encodings;
    int m_chosen = 0;
    DiskVersion m_version;
    QByteArray m_bytes;
    bool m_read = false;
    QString m_error;
    // The chosen encoding's text, once decoded.
    std::optional<Encodings::Decoded> m_decoded;
    QFutureWatcher<Read> m_reading;
    QFutureWatcher<Encodings::Decoded> m_decoding;
};
