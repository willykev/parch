#pragma once

#include "diskfile.h"
#include "exporter.h"
#include "exportpictures.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

// What a web page and a book share: the files they carry beside their text,
// and formulas and diagrams as markup.
namespace ExportWeb {

// The files a page or a book carries: its images and diagrams, each once,
// under a name of its own in one folder.
class Assets {
public:
    enum class Mode {
        Link,    // local files are linked where they are
        Copy,    // copied into a folder beside the page
        Embed,   // written into the page as data: URLs
        Package, // packed with the text, as a book's are
    };
    struct Asset {
        QString name; // within the folder: "a-photo.png"
        QString mediaType;
        QByteArray bytes;
        DiskVersion existing; // File already compared with these bytes, if any.
    };

    // `folder` is how the text reaches the folder: "notes_files/", "images/".
    Assets(Mode mode, const QString &folder, const QString &outputFolder);

    Mode mode() const { return m_mode; }
    // The URL the text gives a local file; empty, with why, when it cannot
    // be read (a link gives its path whether or not it is there).
    QString file(const QString &absolutePath, const QStringList &folders, QString *error);
    // The same of a file that is no picture, such as a bibliography the
    // text names, kept under its own name and suffix in the folder.
    QString copy(const QByteArray &data, const QString &name);
    // The URL of bytes made for the export, such as a diagram, named after
    // `suggested`.
    QString bytes(const QByteArray &data, const QString &mediaType, const QString &suggested);
    const QList<Asset> &assets() const { return m_assets; }

    // The media type of a picture file, from its bytes and name; empty for
    // a file that is no picture a page can show.
    static QString mediaType(const QByteArray &data, const QString &name);

private:
    QString place(const QByteArray &data, const QString &mediaType, const QString &suggested);

    Mode m_mode;
    QString m_folder;
    QString m_outputFolder;
    QList<Asset> m_assets;
    // Each file once: its URL by path, and each name once.
    QHash<QString, QString> m_byPath;
    QHash<QByteArray, QString> m_byHash;
};

// A formula as inline SVG sized in ems, so that it grows with the text
// around it, set on the baseline with its depth below, and named for
// assistive technology by its TeX. `fontSize` is the size it was drawn at.
// Empty when it was not drawn.
QString formula(const ExportPictures::Picture &picture, const QString &tex, bool display, qreal fontSize);

// A diagram's SVG as a browser may show it: Merman's own, or else the plain
// one; empty when there is none to give, or it holds what no page should
// run.
QByteArray diagramSvg(const ExportPictures::Picture &picture);

// Publishes the page after its pictures are in place. Existing entries in
// a marked pictures folder are reused only when their bytes match.
bool writePage(const md::Document &document, const ExportRecipe &recipe, const QByteArray &page, const QString &folder, const Assets &assets, QString *error);
// The marker used by export folders, including older releases.
inline constexpr char kFolderMark[] = ".parch-export";

} // namespace ExportWeb
