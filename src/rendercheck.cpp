#include "rendercheck.h"

#include "renderhelpers.h"
#include "renderservice.h"

#include <QEventLoop>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <optional>

namespace RenderCheck {
namespace {

void collect(const md::Node &node, std::vector<Item> &found)
{
    if (!node.generated) {
        std::optional<RenderKind> kind;
        if (node.type == md::NodeType::MathInline) kind = node.display ? RenderKind::DisplayMath : RenderKind::InlineMath;
        else if (node.type == md::NodeType::MathBlock) kind = RenderKind::DisplayMath;
        else if (node.type == md::NodeType::MermaidBlock) kind = RenderKind::Diagram;
        if (kind) {
            Item item;
            item.file = node.file;
            item.range = node.range;
            item.kind = *kind;
            item.source = node.text;
            found.push_back(std::move(item));
            return;
        }
    }
    for (const md::Node &child : node.children) collect(child, found);
}

} // namespace

std::vector<Item> items(const md::Document &document)
{
    std::vector<Item> all;
    collect(document.root, all);
    std::stable_sort(all.begin(), all.end(), [](const Item &a, const Item &b) {
        return a.file != b.file ? a.file < b.file : a.range.start < b.range.start;
    });
    std::vector<Item> unique;
    QSet<std::pair<int, int>> seen;
    std::vector<std::unique_ptr<DocumentCheck::LineIndex>> lines(document.files.size());
    for (Item &item : all) {
        if (item.file < 0 || size_t(item.file) >= document.files.size()) continue;
        const std::pair<int, int> place{item.file, item.range.start};
        if (seen.contains(place)) continue;
        seen.insert(place);
        auto &index = lines[size_t(item.file)];
        if (!index) index = std::make_unique<DocumentCheck::LineIndex>(document.files[size_t(item.file)].text);
        item.position = index->at(item.range.start);
        unique.push_back(std::move(item));
    }
    return unique;
}

RenderRequest request(const Item &item)
{
    RenderRequest request;
    request.kind = item.kind;
    request.purpose = RenderPurpose::Preview;
    request.source = item.source;
    return request;
}

std::optional<DocumentCheck::Problem> problem(const Item &item, const RenderResult &result)
{
    if (result.ok()) return std::nullopt;
    DocumentCheck::Problem problem;
    problem.file = item.file;
    problem.range = item.range;
    problem.position = item.position;
    problem.severity = md::Severity::Warning;
    const bool math = item.isMath();
    problem.kind = math ? QStringLiteral("math") : QStringLiteral("diagram");
    const QString why = result.diagnostic.trimmed();
    if (result.status == RenderResult::Refused) {
        problem.code = math ? QStringLiteral("invalid-math") : QStringLiteral("invalid-diagram");
        problem.message = math ? QStringLiteral("The formula cannot be drawn: %1").arg(why)
                               : QStringLiteral("The diagram cannot be drawn: %1").arg(why);
    } else {
        problem.code = math ? QStringLiteral("math-not-drawn") : QStringLiteral("diagram-not-drawn");
        problem.message = math ? QStringLiteral("The formula could not be drawn: %1").arg(why)
                               : QStringLiteral("The diagram could not be drawn: %1").arg(why);
    }
    if (why.isEmpty()) problem.message.chop(2);
    return problem;
}

bool canDraw(const HelperSet &helpers, RenderKind kind)
{
    return kind == RenderKind::Diagram ? !helpers.merman.isEmpty() : !helpers.ratex.isEmpty();
}

Batch::Batch(RenderService *service, std::vector<Item> items, QObject *parent)
    : QObject(parent), m_service(service), m_items(std::move(items)), m_problems(m_items.size())
{
}

Batch::~Batch()
{
    cancel();
}

void Batch::start()
{
    if (m_started) return;
    m_started = true;
    if (m_service) {
        connect(m_service, &RenderService::finished, this, &Batch::answered);
        for (size_t i = 0; i < m_items.size(); ++i) m_waiting.insert(m_service->render(request(m_items[i])), i);
    }
    if (m_waiting.isEmpty()) QTimer::singleShot(0, this, &Batch::finished);
}

void Batch::cancel()
{
    if (m_service) {
        disconnect(m_service, nullptr, this, nullptr);
        for (auto it = m_waiting.cbegin(); it != m_waiting.cend(); ++it) m_service->cancel(it.key());
    }
    m_waiting.clear();
    m_service = nullptr;
}

void Batch::answered(quint64 ticket, const RenderResult &result)
{
    const auto waiting = m_waiting.constFind(ticket);
    if (waiting == m_waiting.cend()) return;
    const size_t index = *waiting;
    m_waiting.erase(waiting);
    m_problems[index] = problem(m_items[index], result);
    if (m_waiting.isEmpty()) emit finished();
}

std::vector<DocumentCheck::Problem> Batch::problems() const
{
    std::vector<DocumentCheck::Problem> found;
    for (const auto &each : m_problems)
        if (each) found.push_back(*each);
    return found;
}

std::vector<DocumentCheck::Problem> wait(RenderService &service, const std::vector<Item> &items)
{
    Batch batch(&service, items);
    QEventLoop loop;
    QObject::connect(&batch, &Batch::finished, &loop, &QEventLoop::quit);
    batch.start();
    if (!batch.done()) loop.exec();
    return batch.problems();
}

} // namespace RenderCheck
