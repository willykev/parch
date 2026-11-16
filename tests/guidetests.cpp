#include "guidetests.h"

#include "autosave.h"
#include "backend.h"
#include "commandregistry.h"
#include "document.h"
#include "documentstore.h"
#include "editorbinding.h"
#include "filepicker.h"
#include "history.h"
#include "mdtree.h"
#include "preview.h"
#include "previewbuilder.h"
#include "previewresources.h"
#include "previewstyle.h"
#include "recovery.h"
#include "renderservice.h"
#include "rendertesting.h"
#include "session.h"
#include "settings.h"
#include "sourcebuffer.h"
#include "tabmodel.h"
#include "testfiles.h"
#include "windowcontext.h"
#include "windowtesting.h"

#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <algorithm>

namespace GuideTests {
namespace {

// Runs a command in a window, as its shortcuts, menu and palette do.
void runIn(QQuickWindow *window, const char *command)
{
    QMetaObject::invokeMethod(window, "run", Q_ARG(QVariant, QString::fromLatin1(command)));
}

// The guide as parch carries it.
QString guideText()
{
    QFile file(QStringLiteral(":/guide/markdown-guide.md"));
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

// Answers a save dialog only when a test says so, as a person would.
class SavePicker : public FilePicker {
public:
    int saves = 0;
    QUrl suggested;
    void openDocuments() override {}
    void openFolder() override {}
    void saveDocument(const QUrl &given) override
    {
        ++saves;
        suggested = given;
    }
};

// Every file beneath the folders, with its size and when it last changed.
QMap<QString, QString> filesBeneath(const QStringList &roots)
{
    QMap<QString, QString> files;
    for (const QString &root : roots) {
        if (root.isEmpty()) continue;
        QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QFileInfo info(it.next());
            files.insert(info.filePath(), QStringLiteral("%1 bytes, %2")
                                              .arg(info.size())
                                              .arg(info.lastModified().toMSecsSinceEpoch()));
        }
    }
    return files;
}

// A window of the application, with the guide opened in it on request.
struct GuideWindow {
    // Where its formulas and diagrams are kept once drawn: a folder of the
    // test's own, never the standard cache. It outlives the application.
    QTemporaryDir drawn;
    ShellFixture shell;
    QQuickWindow *window = nullptr;
    Preview *preview = nullptr;

    GuideWindow()
    {
        if (!shell.ok()) return;
        // The real helpers draw them when a build has them, as for the writer.
        shell.backend->setRenderService(
            std::make_unique<RenderService>(renderOptions(realHelpers(), drawn.filePath(QStringLiteral("render")))));
        window = shell.window.get();
        activate(window);
        resizeWindow(window, 1280);
        preview = qobject_cast<Preview *>(shell.object(QStringLiteral("preview")));
    }
    bool ok() const { return shell.ok() && window && preview; }
    TabModel *tabs() const { return shell.context()->tabs(); }
    // Opens the guide as its command does: the document in front then.
    Document *open()
    {
        runIn(window, "markdownGuide");
        Document *front = shell.document();
        return front && front->isScratch() ? front : nullptr;
    }
    // The preview shows the text as it is, built and with its pictures read or failed.
    bool settled(int timeoutMs = 20000) const
    {
        return QTest::qWaitFor(
            [&] { return preview->current() && !preview->working() && preview->resources()->pending() == 0; },
            timeoutMs);
    }
    QStringList lines() const
    {
        QStringList lines;
        for (QTextBlock block = preview->textDocument()->begin(); block.isValid(); block = block.next())
            lines << block.text();
        return lines;
    }
    bool shows(const QString &needle) const
    {
        const QStringList all = lines();
        return std::any_of(all.cbegin(), all.cend(), [&](const QString &line) { return line.contains(needle); });
    }
};

// Where the preview shows a text: in its own text, as a cursor over it, or
// else in an island, as the start of the source of the section that holds
// the island. `source` is where in the guide's text it came from, -1 when
// the preview does not show it.
struct Found {
    QTextCursor cursor;
    int source = -1;
    bool inIsland = false;
};

// Every place the preview shows the text, in reading order, its own text first.
QList<Found> findAll(Preview *preview, const QString &needle)
{
    QList<Found> all;
    for (QTextCursor at = preview->textDocument()->find(needle); !at.isNull();
         at = preview->textDocument()->find(needle, at)) {
        Found found;
        found.cursor = at;
        found.source = preview->sourceAt(at.selectionStart());
        all << found;
    }
    for (const preview::Section &section : preview->sections())
        for (const preview::Island &island : section.islands)
            if (island.document && !island.document->find(needle).isNull()) {
                Found found;
                found.cursor = island.document->find(needle);
                found.source = section.sourceStart;
                found.inIsland = true;
                all << found;
            }
    return all;
}

Found find(Preview *preview, const QString &needle)
{
    const QList<Found> all = findAll(preview, needle);
    return all.isEmpty() ? Found() : all.first();
}

} // namespace

void markdownGuideOpensBesideItsPreview()
{
    // Help → Markdown guide, from the menu and the palette, opens the guide
    // in a tab of its own, its text beside its preview, named for what it is.
    GuideWindow w;
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    CommandRegistry *commands = w.shell.backend->commands();
    QVERIFY(commands->contains(QStringLiteral("markdownGuide")));
    QCOMPARE(commands->info(QStringLiteral("markdownGuide")).title, QStringLiteral("Markdown guide"));
    QCOMPARE(commands->category(QStringLiteral("markdownGuide")), QStringLiteral("Help"));
    QVERIFY(w.shell.object(QStringLiteral("markdownGuideEntry")));
    QCOMPARE(w.shell.object(QStringLiteral("markdownGuideEntry"))->property("command").toString(),
             QStringLiteral("markdownGuide"));
    // Beside the user guide, which has F1; the Markdown guide has no key.
    QVERIFY(w.shell.object(QStringLiteral("userGuideEntry")));
    QVERIFY(commands->shortcuts(QStringLiteral("markdownGuide")).isEmpty());
    const QList<CommandInfo> standard = standardCommands();
    const auto indexOf = [&](const QString &id) {
        return std::find_if(standard.cbegin(), standard.cend(), [&](const CommandInfo &info) { return info.id == id; })
               - standard.cbegin();
    };
    QCOMPARE(indexOf(QStringLiteral("markdownGuide")), indexOf(QStringLiteral("userGuide")) + 1);
    QCOMPARE(commands->category(QStringLiteral("userGuide")), QStringLiteral("Help"));
    Document *launched = w.shell.document();
    QVERIFY(launched && !launched->isScratch());

    Document *guide = w.open();
    QVERIFY(guide);
    QVERIFY(guide != launched);
    // The untouched draft of the launch gave its tab to it, as to any document opened.
    QCOMPARE(w.tabs()->rowCount(), 1);
    QCOMPARE(guide->title(), QStringLiteral("Markdown guide"));
    QCOMPARE(w.tabs()->activeTitle(), QStringLiteral("Markdown guide"));
    QCOMPARE(w.window->title(), QStringLiteral("Markdown guide — parch"));
    QCOMPARE(w.tabs()->activeStatus(), QStringLiteral("Not saved"));
    QCOMPARE(w.shell.item(QStringLiteral("header"))->property("place").toString(), QStringLiteral("Help"));
    QCOMPARE(guide->buffer()->text(), guideText());
    QVERIFY(guide->isDraft());
    QCOMPARE(w.tabs()->activeMode(), TabModel::Split);
    QTRY_VERIFY(w.shell.item(QStringLiteral("editorPane"))->isVisible());
    QVERIFY(w.shell.item(QStringLiteral("previewPane"))->isVisible());
    QTRY_COMPARE(w.window->activeFocusItem(), w.shell.item(QStringLiteral("editor")));
    QVERIFY(w.settled());
    QVERIFY2(w.lines().contains(QStringLiteral("Markdown guide")), qPrintable(w.lines().mid(0, 5).join(u'|')));

    // Asked for again, the guide open already comes to the front as it is.
    guide->buffer()->replace({0, 0}, QStringLiteral("Mine. "));
    runIn(w.window, "newDraft");
    QCOMPARE(w.tabs()->rowCount(), 2);
    QVERIFY(!w.shell.document()->isScratch());
    QCOMPARE(w.open(), guide);
    QCOMPARE(w.tabs()->rowCount(), 2);
    QVERIFY(guide->buffer()->text().startsWith(QStringLiteral("Mine. ")));
}

void markdownGuideRendersEveryPart()
{
    // Every part of the guide is read by the engine and built by the
    // preview: as many blocks and headings as the engine finds in it, and
    // an example of each construct where its part is, formatted as it
    // should be. The included file, the picture and the bibliography are
    // the guide's own resources.
    GuideWindow w;
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    Document *guide = w.open();
    QVERIFY(guide);
    QVERIFY(w.settled());
    Preview *preview = w.preview;
    const QString text = guide->buffer()->text();

    // The parts, from each `## ` heading to the next; the introduction before them.
    QMap<QString, std::pair<int, int>> parts;
    QString previous = QStringLiteral("Introduction");
    int from = 0;
    static const QRegularExpression partHeading(QStringLiteral("^## (.+)$"), QRegularExpression::MultilineOption);
    for (auto it = partHeading.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        parts.insert(previous, {from, int(match.capturedStart())});
        previous = match.captured(1);
        from = int(match.capturedStart());
    }
    parts.insert(previous, {from, int(text.size())});
    QCOMPARE(parts.size(), 21);
    const auto within = [&](const QString &part, int source) {
        return parts.contains(part) && source >= parts[part].first && source < parts[part].second;
    };

    // Blocks and headings, part by part, as the engine reads the guide.
    md::Source source;
    source.directory = QStringLiteral(":/guide");
    const md::Document tree = md::parse(text, md::Options::writer(), source);
    for (auto part = parts.cbegin(); part != parts.cend(); ++part) {
        int blocks = 0, headings = 0;
        for (const md::Node &node : tree.root.children) {
            if (node.range.start < part->first || node.range.start >= part->second) continue;
            const bool nothing = node.type == md::NodeType::FrontMatter || node.type == md::NodeType::LinkDefinition
                                 || node.type == md::NodeType::FootnoteDefinition
                                 || node.type == md::NodeType::CitationDefinition
                                 || node.type == md::NodeType::CommentLine;
            if (!nothing) ++blocks;
            if (node.type == md::NodeType::Heading) ++headings;
        }
        int built = 0, headingMarks = 0;
        for (const preview::Section &section : preview->sections()) {
            if (section.kind != preview::Part::Block || section.sourceStart < part->first
                || section.sourceStart >= part->second)
                continue;
            ++built;
            for (const preview::Mark &mark : section.marks) headingMarks += mark.kind == preview::Mark::Heading;
        }
        const QByteArray name = part.key().toUtf8();
        QVERIFY2(blocks > 1, name);
        QCOMPARE(built, blocks);
        QCOMPARE(headingMarks, headings);
    }
    // Marks, islands and pictures of a part.
    const auto marks = [&](const QString &part, preview::Mark::Kind kind) {
        int count = 0;
        for (const preview::Section &section : preview->sections())
            if (within(part, section.sourceStart))
                for (const preview::Mark &mark : section.marks) count += mark.kind == kind;
        return count;
    };
    const auto islands = [&](const QString &part, preview::Island::Kind kind) {
        int count = 0;
        for (const preview::Section &section : preview->sections())
            if (within(part, section.sourceStart))
                for (const preview::Island &island : section.islands) count += island.kind == kind;
        return count;
    };
    const auto images = [&](const QString &part, const QString &prefix) {
        QList<QTextImageFormat> found;
        for (QTextBlock block = preview->textDocument()->begin(); block.isValid(); block = block.next())
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const QTextCharFormat format = it.fragment().charFormat();
                if (format.isImageFormat() && format.toImageFormat().name().startsWith(prefix)
                    && within(part, preview->sourceAt(it.fragment().position())))
                    found << format.toImageFormat();
            }
        return found;
    };
    // Shown in its part, with its format; a null cursor when it is not.
    const auto at = [&](const QString &part, const QString &needle) {
        for (const Found &found : findAll(preview, needle))
            if (within(part, found.source)) return found.cursor;
        qWarning("“%s” is not shown in “%s”", qPrintable(needle), qPrintable(part));
        return QTextCursor();
    };
    const auto absent = [&](const QString &needle) { return findAll(preview, needle).isEmpty(); };
    using Ink = PreviewStyle::Ink;
    const auto ink = [](const QTextCursor &cursor) { return Ink(cursor.charFormat().intProperty(PreviewStyle::InkProperty)); };

    // The introduction, under the guide's heading.
    QTextCursor found = at(QStringLiteral("Introduction"), QStringLiteral("Markdown guide"));
    QVERIFY(!found.isNull());
    QCOMPARE(found.blockFormat().headingLevel(), 1);
    found = at(QStringLiteral("Introduction"), QStringLiteral("Save as…"));
    QVERIFY(!found.isNull() && found.charFormat().fontWeight() >= QFont::Bold);

    // Paragraphs and line breaks: soft breaks are spaces, hard breaks
    // break the line, and a rule is drawn.
    QString part = QStringLiteral("Paragraphs and line breaks");
    QVERIFY(!at(part, QStringLiteral("these three lines are read as one line.")).isNull());
    found = at(part, QStringLiteral("like this, or with a backslash"));
    QVERIFY(!found.isNull());
    QVERIFY2(found.block().text().contains(QStringLiteral("two spaces") + QChar::LineSeparator
                                           + QStringLiteral("like this, or with a backslash")
                                           + QChar::LineSeparator + QStringLiteral("like this.")),
             qPrintable(found.block().text()));
    QCOMPARE(marks(part, preview::Mark::Rule), 1);

    // Headings of every kind, their marks never shown, and what is not one.
    part = QStringLiteral("Headings");
    QCOMPARE(at(part, QStringLiteral("A third-level heading")).blockFormat().headingLevel(), 3);
    found = at(part, QStringLiteral("A fourth-level heading"));
    QCOMPARE(found.blockFormat().headingLevel(), 4);
    QCOMPARE(found.block().text(), QStringLiteral("A fourth-level heading"));
    found = at(part, QStringLiteral("A heading with a label"));
    QCOMPARE(found.blockFormat().headingLevel(), 3);
    QCOMPARE(found.block().text(), QStringLiteral("A heading with a label"));
    QCOMPARE(at(part, QStringLiteral("the labelled heading")).charFormat().anchorHref(),
             QStringLiteral("#a-heading-with-a-label"));
    found = at(part, QStringLiteral("#5 bolts and a washer"));
    QVERIFY(!found.isNull());
    QCOMPARE(found.blockFormat().headingLevel(), 0);
    QCOMPARE(at(part, QStringLiteral("Underlined heading")).blockFormat().headingLevel(), 2);

    // Emphasis, and what stays as written.
    part = QStringLiteral("Emphasis");
    found = at(part, QStringLiteral("both at once"));
    QVERIFY(found.charFormat().fontItalic() && found.charFormat().fontWeight() >= QFont::Bold);
    found = at(part, QStringLiteral("bold with "));
    QVERIFY(found.charFormat().fontWeight() >= QFont::Bold && !found.charFormat().fontItalic());
    QVERIFY(!at(part, QStringLiteral("snake_case_names")).isNull());
    QVERIFY(!at(part, QStringLiteral("This *never closes")).isNull());
    QVERIFY(!at(part, QStringLiteral("2 * 3 * 4")).isNull());
    found = at(part, QStringLiteral("*not italic*"));
    QVERIFY(!found.isNull() && !found.charFormat().fontItalic());

    part = QStringLiteral("Strikethrough, highlight, superscript and subscript");
    QVERIFY(at(part, QStringLiteral("no longer true")).charFormat().fontStrikeOut());
    QCOMPARE(at(part, QStringLiteral("worth remembering")).charFormat().intProperty(PreviewStyle::PaperProperty),
             int(PreviewStyle::MarkPaper));
    QCOMPARE(at(part, QStringLiteral("(a+b)")).charFormat().verticalAlignment(), QTextCharFormat::AlignSuperScript);
    QCOMPARE(at(part, QStringLiteral("i,j")).charFormat().verticalAlignment(), QTextCharFormat::AlignSubScript);
    QVERIFY(!at(part, QStringLiteral(" words^")).isNull());

    // Links lead where they say; the picture beside the guide is shown,
    // and one that is not there says so.
    part = QStringLiteral("Links and images");
    QCOMPARE(at(part, QStringLiteral("an example site")).charFormat().anchorHref(), QStringLiteral("https://example.org"));
    QCOMPARE(at(part, QStringLiteral("back to the")).charFormat().anchorHref(), QStringLiteral("#headings"));
    QVERIFY(at(part, QStringLiteral("someone@example.org")).charFormat().anchorHref().startsWith(QStringLiteral("mailto:")));
    QTRY_COMPARE(images(part, QStringLiteral("parch:image/")).size(), 1);
    QCOMPARE(images(part, QStringLiteral("parch:image/")).first().width(), 320.0);
    QVERIFY(!at(part, QStringLiteral("Image not shown: A picture that is missing")).isNull());

    // Lists count from one in this profile, and tasks have their boxes.
    part = QStringLiteral("Lists and task lists");
    QVERIFY(at(part, QStringLiteral("Soft ones, indented")).block().text().startsWith(QStringLiteral("◦")));
    QVERIFY2(at(part, QStringLiteral("Seventh?")).block().text().startsWith(QStringLiteral("1.")),
             qPrintable(at(part, QStringLiteral("Seventh?")).block().text()));
    QCOMPARE(images(part, QStringLiteral("parch:task/")).size(), 3);
    QVERIFY(!at(part, QStringLiteral("1984. A good year")).isNull());

    part = QStringLiteral("Quotes");
    QVERIFY(marks(part, preview::Mark::QuoteBar) >= 2);
    QVERIFY(!at(part, QStringLiteral("Write the way you talk, and the quote goes on.")).isNull());

    // Four code blocks, the Python one coloured as Python, and inline code.
    part = QStringLiteral("Code");
    QCOMPARE(islands(part, preview::Island::Code), 4);
    QCOMPARE(ink(at(part, QStringLiteral("print(\"hello\")"))), PreviewStyle::Code);
    QVERIFY(find(preview, QStringLiteral("def greet(name):")).inIsland);
    QVERIFY(find(preview, QStringLiteral("Inside the outer fence, this is plain text.")).inIsland);

    // Two tables, and pipes that are no table.
    part = QStringLiteral("Tables");
    QCOMPARE(islands(part, preview::Island::Table), 2);
    QVERIFY(within(part, find(preview, QStringLiteral("Notebooks")).source));
    QVERIFY(!at(part, QStringLiteral("| Not | a table |")).isNull());

    // Notes, at the end, and a label with no note in view.
    part = QStringLiteral("Footnotes");
    QVERIFY(!at(part, QStringLiteral("[^nowhere]")).isNull());
    QVERIFY(w.shows(QStringLiteral("This is the footnote's text.")));
    QVERIFY(w.shows(QStringLiteral("This one is written right where it is used.")));

    // Citations as the style words them, from the bibliography beside the guide.
    part = QStringLiteral("Citations");
    QVERIFY(!at(part, QStringLiteral("(Vale, 2019, p. 12)")).isNull());
    QVERIFY(!at(part, QStringLiteral("(see Okafor & Lindqvist, 2021, pp. 44–45; also Vale, 2019)")).isNull());
    QVERIFY(!at(part, QStringLiteral("Vale says so too (2019)")).isNull());
    QVERIFY(!at(part, QStringLiteral("Okafor & Lindqvist (2021) found the same")).isNull());
    QCOMPARE(ink(at(part, QStringLiteral("nobody2020"))), PreviewStyle::Danger);
    QVERIFY(w.lines().contains(QStringLiteral("Vale, I. (2019). The patient page. Lantern Hill Books.")));

    // Formulas and diagrams are drawn by helpers, which a test run may not
    // have: each is in its place, drawn or saying why it is not.
    const auto builtAt = [&](const QString &start) {
        const int at = int(text.indexOf(start));
        return at >= 0 && std::any_of(preview->sections().cbegin(), preview->sections().cend(),
                                      [&](const preview::Section &section) { return section.sourceStart == at; });
    };
    part = QStringLiteral("Math");
    QVERIFY(builtAt(QStringLiteral("$$\nx_{1,2}")));
    QVERIFY(builtAt(QStringLiteral("Dollar signs hold math")));
    QVERIFY(!at(part, QStringLiteral("$5 and $10 stay as they are")).isNull());
    QVERIFY(builtAt(QStringLiteral("```mermaid")));
    // With the helpers, drawn: the inline formula a picture in its line,
    // the display one an island of its own, the diagram a picture.
    if (!realHelpers().ratex.isEmpty()) {
        QTRY_COMPARE(images(part, QStringLiteral("parch:math/")).size(), 1);
        QTRY_COMPARE(islands(part, preview::Island::Math), 1);
        QTRY_COMPARE(images(QStringLiteral("Diagrams"), QStringLiteral("parch:diagram/")).size(), 1);
    }

    // Variables from the front matter, which is not shown, and what is not front matter.
    part = QStringLiteral("Front matter and variables");
    QVERIFY(!at(part, QStringLiteral("this is The Markdown guide, by parch.")).isNull());
    QVERIFY(!at(part, QStringLiteral("[%subtitle]")).isNull());
    QCOMPARE(at(part, QStringLiteral("title: Not metadata")).blockFormat().headingLevel(), 2);
    QVERIFY(absent(QStringLiteral("csl: apa")));

    // The included file is read from beside the guide; a missing one stays a line.
    part = QStringLiteral("Includes");
    QVERIFY(w.shows(QStringLiteral("This paragraph lives in a file of its own")));
    QVERIFY(!at(part, QStringLiteral("missing-chapter.md")).isNull());

    part = QStringLiteral("Comments and HTML");
    QVERIFY(absent(QStringLiteral("An HTML comment is never shown")));
    QVERIFY(absent(QStringLiteral("This line stays in the text")));
    QCOMPARE(ink(at(part, QStringLiteral("Ctrl"))), PreviewStyle::Code);
    QVERIFY(!at(part, QStringLiteral("<marquee>no scrolling here</marquee>")).isNull());
    QVERIFY(!at(part, QStringLiteral("alert(")).isNull());

    part = QStringLiteral("Definition lists");
    QVERIFY(!at(part, QStringLiteral("The blank space around the text of a page.")).isNull());
    QVERIFY(!at(part, QStringLiteral("Also the space between columns.")).isNull());

    QCOMPARE(marks(QStringLiteral("Page breaks"), preview::Mark::PageBreak), 1);

    part = QStringLiteral("Contents");
    QCOMPARE(at(part, QStringLiteral("Strikethrough, highlight, superscript and subscript")).charFormat().anchorHref(),
             QStringLiteral("#strikethrough-highlight-superscript-and-subscript"));

    part = QStringLiteral("Profiles and the Writer extensions");
    QVERIFY(!at(part, QStringLiteral("follows CommonMark with GitHub's tables")).isNull());
}

void markdownGuideKeepsNothingOnDisk()
{
    // Editing the guide changes its preview and nothing on disk: no draft,
    // no recovery, no history, and the session never names it.
    GuideWindow w;
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    Document *guide = w.open();
    QVERIFY(guide);
    const QString id = guide->id();
    QVERIFY(w.settled());
    // What opening the guide changed in the session is written first.
    QTest::qWait(Backend::SessionDelayMs + 500);
    const QStringList roots{w.shell.root.path(),
                            QDir::homePath(),
                            qEnvironmentVariable("XDG_CONFIG_HOME"),
                            qEnvironmentVariable("XDG_DATA_HOME"),
                            qEnvironmentVariable("XDG_STATE_HOME"),
                            qEnvironmentVariable("XDG_CACHE_HOME"),
                            qEnvironmentVariable("XDG_RUNTIME_DIR")};
    const QMap<QString, QString> before = filesBeneath(roots);

    // Typed into, and ticked in the preview.
    QQuickItem *editor = w.shell.item(QStringLiteral("editor"));
    editor->forceActiveFocus();
    QTRY_COMPARE(w.window->activeFocusItem(), editor);
    w.shell.binding()->restore(0, 0, 0);
    typeText(w.window, QStringLiteral("Typed here. "));
    QCOMPARE(guide->state(), Document::State::Modified);
    QCOMPARE(w.tabs()->activeStatus(), QStringLiteral("Not saved"));
    const int box = int(guide->buffer()->text().indexOf(QStringLiteral("- [ ] Send it")));
    guide->buffer()->replace({box + 3, 1}, QStringLiteral("x"));
    QTRY_VERIFY(w.shows(QStringLiteral("Typed here.")));
    QVERIFY(w.settled());
    // Read as Standard Markdown, as its last part suggests, for as long as it is open.
    QVERIFY(w.shell.backend->documentSettings()->setProfile(guide, QStringLiteral("document"), QStringLiteral("standard")));
    QTRY_VERIFY(w.shows(QStringLiteral("==worth remembering==")));
    QCOMPARE(w.shell.backend->documentSettings()->describe(guide).value(QStringLiteral("profile")).toString(),
             QStringLiteral("standard"));
    QVERIFY(w.settled());
    // Long enough for any autosave, recovery snapshot or session write.
    QTest::qWait(std::max({Autosave::SaveIdleMs, Autosave::RecoveryMaxMs, Backend::SessionDelayMs}) + 1000);

    const QMap<QString, QString> after = filesBeneath(roots);
    QStringList changed;
    for (auto it = after.cbegin(); it != after.cend(); ++it)
        if (before.value(it.key()) != it.value()) changed << it.key() + QStringLiteral(": ") + it.value();
    for (auto it = before.cbegin(); it != before.cend(); ++it)
        if (!after.contains(it.key())) changed << it.key() + QStringLiteral(": gone");
    QVERIFY2(changed.isEmpty(), qPrintable(changed.join(u'\n')));
    const AppPaths paths = w.shell.paths();
    for (const Recovery::Entry &entry : Recovery(paths).pending()) QVERIFY(entry.documentId != id);
    QVERIFY(w.shell.backend->history()->versions(id).isEmpty());
    QVERIFY(!QFileInfo::exists(paths.drafts() + u'/' + id));
    for (const WindowState &window : Session::load(paths).windows)
        for (const ViewState &tab : window.tabs) QVERIFY(tab.documentId != id);
    QVERIFY(guide->isScratch());
}

void markdownGuideClosesWithoutAsking()
{
    // Closing the guide asks nothing, whatever was written in it, and neither
    // does quitting; opened again, it starts fresh.
    GuideWindow w;
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    // The launch's draft keeps its own tab.
    Document *draft = w.shell.document();
    draft->buffer()->replace({0, 0}, QStringLiteral("x"));
    draft->buffer()->undo();
    QPointer<Document> guide = w.open();
    QVERIFY(guide);
    QCOMPARE(w.tabs()->rowCount(), 2);
    const QString id = guide->id();
    guide->buffer()->replace({0, 0}, QStringLiteral("Changed. "));
    QCOMPARE(guide->state(), Document::State::Modified);

    // Quitting is not held up by it.
    QSignalSpy quits(w.shell.backend.get(), &Backend::quitRequested);
    w.shell.backend->quit();
    QCOMPARE(quits.count(), 1);
    QVERIFY(w.shell.backend->problem().isEmpty());

    runIn(w.window, "closeTab");
    QTRY_COMPARE(w.tabs()->rowCount(), 1);
    QVERIFY(!w.shell.object(QStringLiteral("closeConfirmation"))->property("visible").toBool());
    QVERIFY(w.shell.backend->problem().isEmpty());
    QTRY_VERIFY(guide.isNull());
    for (Document *open : w.shell.backend->store()->documents()) QVERIFY(open->id() != id);
    QCOMPARE(w.shell.document(), draft);

    // Fresh again, beside its preview.
    Document *again = w.open();
    QVERIFY(again);
    QVERIFY(again->id() != id);
    QCOMPARE(again->buffer()->text(), guideText());
    QCOMPARE(again->state(), Document::State::Clean);
    QCOMPARE(w.tabs()->activeMode(), TabModel::Split);
}

void markdownGuideIsSavedAsACopy()
{
    // Saving the guide asks where, as for a draft, and writes a copy there,
    // which is then an ordinary document: saved as it is edited, kept in
    // history, a tab of the session. The guide opened again is the guide.
    GuideWindow w;
    QVERIFY2(w.ok(), qPrintable(w.shell.error));
    SavePicker picker;
    w.shell.backend->setFilePicker(&picker);
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString place = QFileInfo(folder.path()).canonicalFilePath();
    w.shell.settings()->setLastFolder(place);
    Document *guide = w.open();
    QVERIFY(guide);
    const QString id = guide->id();
    const QString end = QStringLiteral("\nMy own line.\n");
    guide->buffer()->replace({int(guide->buffer()->length()), 0}, end);

    runIn(w.window, "save");
    QCOMPARE(picker.saves, 1);
    QCOMPARE(picker.suggested.fileName(), QStringLiteral("Markdown guide.md"));
    const QString target = place + QStringLiteral("/My guide.md");
    emit picker.saveSelected(QUrl::fromLocalFile(target));
    QTRY_COMPARE(readFile(target), (guideText() + end).toUtf8());
    QTRY_VERIFY(!guide->isScratch());
    QCOMPARE(w.shell.document(), guide);
    QCOMPARE(guide->id(), id);
    QCOMPARE(guide->path(), target);
    QVERIFY(!guide->isDraft());
    QTRY_COMPARE(w.tabs()->activeTitle(), QStringLiteral("My guide.md"));
    QCOMPARE(w.tabs()->activeStatus(), QStringLiteral("Saved"));
    QVERIFY(w.shell.item(QStringLiteral("header"))->property("place").toString() != QStringLiteral("Help"));
    QVERIFY(w.shell.backend->history()->versions(id).size() >= 1);

    // An ordinary document: its edits reach its file by themselves.
    guide->buffer()->replace({0, 0}, QStringLiteral("Kept. "));
    QTRY_VERIFY_WITH_TIMEOUT(readFile(target).startsWith("Kept. "), Autosave::SaveMaxMs + 3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        [&] {
            for (const WindowState &window : Session::load(w.shell.paths()).windows)
                for (const ViewState &tab : window.tabs)
                    if (tab.path == target) return true;
            return false;
        }(),
        Backend::SessionDelayMs + 3000);

    // The guide again is the guide, beside the copy.
    Document *fresh = w.open();
    QVERIFY(fresh && fresh != guide);
    QCOMPARE(fresh->buffer()->text(), guideText());
    QCOMPARE(w.tabs()->rowCount(), 2);
    QCOMPARE(readFile(target).left(6), QByteArray("Kept. "));
}

} // namespace GuideTests
