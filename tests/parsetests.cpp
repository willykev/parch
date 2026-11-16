#include "parsetests.h"

#include "apppaths.h"
#include "dialectchoice.h"
#include "document.h"
#include "documentchecker.h"
#include "documentparse.h"
#include "documentstore.h"
#include "editordecorations.h"
#include "exports.h"
#include "markdownstyling.h"
#include "mdtree.h"
#include "outline.h"
#include "preview.h"
#include "printpreview.h"
#include "sourcebuffer.h"
#include "spellchecker.h"
#include "spellinglayer.h"
#include "tabmodel.h"
#include "testfiles.h"
#include "windowcontext.h"
#include "windowtesting.h"
#include "wordcounter.h"

#include <QFileInfo>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {

// A small Hunspell dictionary that knows the words of the texts below.
void writeWords(const QString &folder)
{
    writeFile(folder + QStringLiteral("/en_US.aff"), "SET UTF-8\nTRY esianrtolcdugmphbyfvkwz'\n");
    const QStringList words = QString::fromLatin1(
                                  "and heading here in item items link more one opening part prose some text three "
                                  "two words")
                                  .split(u' ');
    writeFile(folder + QStringLiteral("/en_US.dic"),
              (QString::number(words.size()) + u'\n' + words.join(u'\n') + u'\n').toUtf8());
}

std::vector<DecorationSpan> joinedRuns(EditorDecorations *decorations)
{
    std::vector<DecorationSpan> joined;
    for (const DecorationSpan &span : decorations->spans(MarkdownStyling::layerName())) {
        if (!joined.empty() && joined.back().end == span.start && joined.back().style == span.style)
            joined.back().end = span.end;
        else
            joined.push_back(span);
    }
    return joined;
}

QStringList underlined(EditorDecorations *decorations, const Document *document)
{
    QStringList words;
    const QString text = document->buffer()->text();
    for (const DecorationSpan &span : decorations->spans(SpellingLayer::layerName()))
        words << text.mid(span.start, span.end - span.start);
    return words;
}

QStringList textsOf(const QList<Heading> &headings)
{
    QStringList texts;
    for (const Heading &heading : headings) texts << heading.text;
    return texts;
}

} // namespace

namespace ParseTests {

void everyViewReadsOneParseOfEachRevision()
{
    // A window with every view of the text there is: live formatting and
    // spelling in the editor, the preview beside it, the word count in the
    // status line, and the outline.
    QTemporaryDir files, dictionaries;
    writeWords(dictionaries.path());
    const QString path = files.filePath(QStringLiteral("shared.md"));
    QString text = QStringLiteral("# Opening\n\nSome prose here, and some more.\n\n## Part two\n\n"
                                  "- one item\n- two **items**\n\nA [link](https://example.org) in text.\n");
    writeFile(path, text.toUtf8());
    ShellFixture shell({.files = {path}, .dictionaries = dictionaries.path()});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 1280);
    Document *document = shell.document();
    QVERIFY(document && document->path() == path);
    shell.context()->tabs()->setActiveMode(TabModel::Split);
    auto *preview = qobject_cast<Preview *>(shell.object(QStringLiteral("preview")));
    auto *styling = qobject_cast<MarkdownStyling *>(shell.object(QStringLiteral("styling")));
    auto *spelling = qobject_cast<SpellingLayer *>(shell.object(QStringLiteral("spelling")));
    auto *decorations = qobject_cast<EditorDecorations *>(shell.object(QStringLiteral("decorations")));
    auto *counter = qobject_cast<WordCounter *>(shell.object(QStringLiteral("wordCounter")));
    QVERIFY(preview && styling && spelling && decorations && counter);
    QTRY_VERIFY(preview->active());
    QTRY_VERIFY(shell.backend->spelling()->active());
    OutlineModel outline;
    outline.setDocument(document);
    outline.setActive(true);
    DocumentParse *parse = document->parse();

    // Every view shows the text as it is: the preview, the styling, the
    // underlines, the outline and the count, each from its own worker.
    struct Expected {
        std::vector<DecorationSpan> runs;
        QStringList headings;
        int words = 0;
        QString misspelled;
    };
    const auto expect = [](const QString &text, const QString &misspelled) {
        return Expected{markdownStyleRuns(md::parse(text)), textsOf(Outline::headings(text)), WordCounter::count(text),
                        misspelled};
    };
    const auto settled = [&](const Expected &expected) {
        return QTest::qWaitFor(
            [&] {
                decorations->flush();
                const Revision now = document->buffer()->revision();
                return !parse->parsing() && preview->current() && !preview->working()
                       && preview->shownRevision() == now && !styling->busy()
                       && joinedRuns(decorations) == expected.runs && !spelling->checking()
                       && (expected.misspelled.isEmpty()
                               ? underlined(decorations, document).isEmpty()
                               : underlined(decorations, document) == QStringList{expected.misspelled})
                       && outline.ready() && textsOf(outline.headings()) == expected.headings && !counter->counting()
                       && counter->words() == expected.words;
            },
            10000);
    };
    QVERIFY(settled(expect(text, {})));

    // Each edit, one revision, is parsed once, by the document, and no view
    // parses it again: not on its worker, and not on this thread. Each is
    // put before the text it names, or at the end.
    const QList<std::pair<QString, QString>> edits{{QStringLiteral("# Opening"), QStringLiteral("# Zzqx heading\n\n")},
                                                   {QString(), QStringLiteral("\nThree more words.\n")},
                                                   {QStringLiteral("Some"), QStringLiteral("Prose ")}};
    QString misspelled;
    const int droppedBefore = parse->dropped();
    for (const auto &[before, inserted] : edits) {
        QString next = document->buffer()->text();
        const int start = before.isEmpty() ? int(next.size()) : int(next.indexOf(before));
        QVERIFY(start >= 0);
        next.insert(start, inserted);
        if (inserted.contains(u"Zzqx")) misspelled = QStringLiteral("Zzqx");
        const Expected expected = expect(next, misspelled);
        const quint64 parsedBefore = md::parseCount();
        const int parsesBefore = parse->parses();
        document->buffer()->replace({start, 0}, inserted);
        QVERIFY2(settled(expected), qPrintable(inserted));
        QCOMPARE(document->buffer()->text(), next);
        QCOMPARE(parse->parses() - parsesBefore, 1);
        QCOMPARE(md::parseCount() - parsedBefore, quint64(1));
        QCOMPARE(parse->current()->revision, document->buffer()->revision());
    }
    QCOMPARE(parse->dropped(), droppedBefore);
}

void staleParsesAreNeverHandedOver()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    DocumentStore store(AppPaths::under(root.path()));
    Document *document = store.newDraft();
    SourceBuffer *buffer = document->buffer();
    buffer->replace({0, 0}, QStringLiteral("# One\n\nSome text.\n"));
    DocumentParse *parse = document->parse();
    // Whatever is handed over is the text as it is at that moment.
    QStringList wrong;
    QList<Revision> handed;
    QObject::connect(parse, &DocumentParse::parsed, parse, [&](const ParsedTextPtr &parsed) {
        handed << parsed->revision;
        if (parsed->revision != buffer->revision() || parsed->text != buffer->text())
            wrong << QStringLiteral("revision %1 handed over at %2").arg(parsed->revision).arg(buffer->revision());
    });
    // Parsing takes long enough to be overtaken.
    DocumentParse::setWorkerDelay(150);
    const auto restore = qScopeGuard([] { DocumentParse::setWorkerDelay(0); });

    QVERIFY(!parse->current());
    QVERIFY(!parse->request());
    QVERIFY(parse->parsing());
    // Asked again while it runs: it is not started twice.
    QVERIFY(!parse->request());
    buffer->replace({int(buffer->length()), 0}, QStringLiteral("More."));
    QTRY_COMPARE_WITH_TIMEOUT(handed.size(), 1, 5000);
    QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(u'\n')));
    QCOMPARE(handed.first(), buffer->revision());
    // The first parse was of the text before the edit: dropped, and the
    // text parsed again at once.
    QCOMPARE(parse->parses(), 2);
    QCOMPARE(parse->dropped(), 1);
    const ParsedTextPtr current = parse->current();
    QVERIFY(current);
    QCOMPARE(current->text, buffer->text());
    QCOMPARE(current->document().headings.size(), size_t(1));
    // The current text is not parsed again.
    QCOMPARE(parse->request(), current);
    QCOMPARE(parse->parses(), 2);

    // Edits faster than parsing: only the text as it ends is handed over.
    QVERIFY(!parse->current() || parse->current() == current);
    buffer->replace({0, 0}, QStringLiteral("A"));
    QVERIFY(!parse->current());
    QVERIFY(!parse->request());
    for (int i = 0; i < 5; ++i) {
        QTest::qWait(60);
        buffer->replace({0, 0}, QStringLiteral("B"));
    }
    QTRY_COMPARE_WITH_TIMEOUT(parse->parsing(), false, 5000);
    QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(u'\n')));
    QCOMPARE(handed.last(), buffer->revision());
    QVERIFY(parse->dropped() >= 2);
    QCOMPARE(parse->current()->text, buffer->text());

    // Files the text was read with changed: the next request reads it again.
    const int parses = parse->parses();
    parse->invalidate();
    QVERIFY(!parse->current());
    QVERIFY(!parse->request());
    QTRY_VERIFY_WITH_TIMEOUT(parse->current(), 5000);
    QCOMPARE(parse->parses(), parses + 1);
    QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(u'\n')));
}

void aChangeOfDialectParsesAgain()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    DocumentStore store(AppPaths::under(root.path()));
    Document *document = store.newDraft();
    document->buffer()->replace({0, 0}, QStringLiteral("A ==marked== word and $x^2$.\n"));
    DocumentParse *parse = document->parse();
    QSignalSpy handed(parse, &DocumentParse::parsed);
    const auto hasNode = [](const md::Node &node, md::NodeType type, const auto &self) -> bool {
        if (node.type == type) return true;
        for (const md::Node &child : node.children)
            if (self(child, type, self)) return true;
        return false;
    };

    QVERIFY(!parse->request());
    QVERIFY(handed.wait(5000));
    ParsedTextPtr writer = parse->current();
    QVERIFY(writer);
    QCOMPARE(writer->reading.dialect.options, md::Options::writer());
    QVERIFY(hasNode(writer->document().root, md::NodeType::Highlight, hasNode));

    // Another profile: the same revision reads otherwise, and is parsed again.
    Dialect::Layers standard;
    standard.document.profile = md::Profile::Standard;
    const Revision revision = document->buffer()->revision();
    document->setSavedDialect(standard);
    QVERIFY(!parse->current());
    QVERIFY(!parse->request());
    QVERIFY(handed.wait(5000));
    const ParsedTextPtr read = parse->current();
    QVERIFY(read && read != writer);
    QCOMPARE(read->revision, revision);
    QCOMPARE(read->reading.dialect.options, md::Options::standard());
    QVERIFY(!hasNode(read->document().root, md::NodeType::Highlight, hasNode));

    // A change while a parse runs: what it read in the old dialect is
    // dropped, never handed over, and the text is read in the new one.
    DocumentParse::setWorkerDelay(150);
    const auto restore = qScopeGuard([] { DocumentParse::setWorkerDelay(0); });
    document->buffer()->replace({0, 0}, QStringLiteral("Now "));
    handed.clear();
    QVERIFY(!parse->request());
    QVERIFY(parse->parsing());
    const int dropped = parse->dropped();
    document->setSavedDialect({});
    QTRY_COMPARE_WITH_TIMEOUT(handed.size(), 1, 5000);
    QCOMPARE(parse->dropped(), dropped + 1);
    const ParsedTextPtr back = handed.first().first().value<ParsedTextPtr>();
    QCOMPARE(back->reading.dialect.options, md::Options::writer());
    QVERIFY(hasNode(back->document().root, md::NodeType::Highlight, hasNode));
    QCOMPARE(parse->current(), back);
}

void viewsReadTheDocumentsOwnText()
{
    // A document read with the chapter it includes: the outline lists the
    // chapter's heading at its include, as the preview shows it, while the
    // count and the styling read the document's own text, as when it was
    // parsed alone.
    QTemporaryDir files;
    const QString chapter = files.filePath(QStringLiteral("chapter.md"));
    writeFile(chapter, "# Chapter heading\n\nThree more words in a chapter, and [a link][there].\n\n"
                       "[there]: https://example.org/there\n");
    const QString path = files.filePath(QStringLiteral("book.md"));
    const QString text = QStringLiteral("# Opening\n\nSome prose[^1] and [a link][here].\n\nchapter.md\n\nThe end.\n\n"
                                        "[^1]: A note.\n[here]: https://example.org/here\n");
    writeFile(path, text.toUtf8());
    const std::vector<DecorationSpan> alone = markdownStyleRuns(md::parse(text));
    const int words = WordCounter::count(text);
    ShellFixture shell({.files = {path}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    Document *document = shell.document();
    QVERIFY(document && document->path() == path);
    auto *decorations = qobject_cast<EditorDecorations *>(shell.object(QStringLiteral("decorations")));
    auto *styling = qobject_cast<MarkdownStyling *>(shell.object(QStringLiteral("styling")));
    auto *counter = qobject_cast<WordCounter *>(shell.object(QStringLiteral("wordCounter")));
    QVERIFY(decorations && styling && counter);
    OutlineModel outline;
    outline.setDocument(document);
    outline.setActive(true);

    QTRY_VERIFY(outline.ready());
    const ParsedTextPtr parsed = document->parse()->current();
    QVERIFY(parsed);
    // The parse holds the chapter, which the preview and the outline show ...
    QCOMPARE(parsed->document().files.size(), size_t(2));
    QCOMPARE(parsed->document().headings.size(), size_t(2));
    const QList<Heading> headings = outline.headings();
    QCOMPARE(textsOf(headings), (QStringList{QStringLiteral("Opening"), QStringLiteral("Chapter heading")}));
    QCOMPARE(headings.at(1).file, QFileInfo(chapter).canonicalFilePath());
    QCOMPARE(headings.at(1).position, int(text.indexOf(u"chapter.md")));
    QVERIFY(headings.at(0).file.isEmpty());
    // ... and the count and the styling are of the document's own text, as
    // they were when it was parsed alone.
    QTRY_COMPARE(counter->words(), words);
    QCOMPARE(markdownStyleRuns(parsed->document()), alone);
    QVERIFY(QTest::qWaitFor(
        [&] {
            decorations->flush();
            return !styling->busy() && joinedRuns(decorations) == alone;
        },
        5000));
}

void checkingAndExportingReadTheSharedParse()
{
    // Check document, the Export dialog's limitations, the print preview and
    // an export read the document's parse of its text: once it is parsed,
    // none of them parses it again.
    QTemporaryDir files;
    const QString path = files.filePath(QStringLiteral("checked.md"));
    writeFile(path, "# Checked\n\nA picture that is not there: ![gone](gone.png)\n\nchapter.md\n");
    writeFile(files.filePath(QStringLiteral("chapter.md")), "## The chapter\n\nIts own words.\n");
    ShellFixture shell({.files = {path}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    Document *document = shell.document();
    QVERIFY(document && document->path() == path);
    DocumentParse *parse = document->parse();
    QTRY_VERIFY(parse->current() && !parse->parsing());
    QCOMPARE(parse->current()->document().files.size(), size_t(2));
    const quint64 parsedBefore = md::parseCount();
    const int parsesBefore = parse->parses();

    DocumentChecker checker;
    checker.setBackend(shell.backend.get());
    checker.check(document);
    QTRY_VERIFY(!checker.checking());
    bool missing = false;
    for (const QVariant &row : checker.problems())
        missing = missing || row.toMap().value(QStringLiteral("code")).toString() == u"unresolved-asset";
    QVERIFY(missing);

    Exports *exports = shell.backend->exports();
    exports->read(document);
    QTRY_VERIFY(!exports->reading());
    QVERIFY(!exports->limitations(QStringLiteral("html"), {}).contains(
        QStringLiteral("A file it includes cannot be read, so it cannot be exported until it can.")));

    PrintPreview *pages = exports->preview();
    pages->show(document, QStringLiteral("export"), {});
    QTRY_VERIFY_WITH_TIMEOUT(!pages->busy() && pages->pageCount() > 0, 20000);

    QSignalSpy exported(exports, &Exports::finished);
    const QString output = files.filePath(QStringLiteral("checked.html"));
    exports->exportTo(document, QStringLiteral("html"), {}, output);
    QVERIFY(exported.wait(20000));
    QCOMPARE(exported.first().first().toBool(), true);
    QVERIFY(readFile(output).contains("Its own words."));

    QCOMPARE(parse->parses(), parsesBefore);
    QCOMPARE(md::parseCount(), parsedBefore);
}

} // namespace ParseTests
