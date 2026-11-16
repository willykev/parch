#pragma once

#include <QString>
#include <QUnhandledException>

#include <atomic>
#include <exception>
#include <new>

// A job on a worker can fail: a document, a bibliography or a picture made
// to harm can make one run out of memory, and any part of the reading can
// throw. Every job of the document's parse, the preview, the exports, the
// print preview, Check document and the searches catches what it throws and
// hands back the reason instead: the view that waited for it is told, says
// why, and is ready for the next try, rather than the process ending or the
// view waiting for good.
namespace WorkerFailure {

// What a failure says: that memory ran out, or what the exception says.
inline QString reason(std::exception_ptr failure)
{
    try {
        if (failure) std::rethrow_exception(failure);
    } catch (const QUnhandledException &unhandled) {
        if (unhandled.exception()) return reason(unhandled.exception());
    } catch (const std::bad_alloc &) {
        return QStringLiteral("there was not enough memory to read it");
    } catch (const std::exception &thrown) {
        const QString what = QString::fromLocal8Bit(thrown.what());
        if (!what.isEmpty()) return what;
    } catch (...) {
    }
    return QStringLiteral("it could not be read");
}

// The jobs a test can make fail.
enum class Job : unsigned {
    Parse = 1,    // the document's parse
    Preview = 2,  // the preview's work on a parse
    Export = 4,   // an export or a print
    Check = 8,    // Check document
    Search = 16,  // a search of a folder or of the document
    Picture = 32, // a picture the preview reads
};

inline std::atomic<unsigned> failing{0};

// For tests: every job of the kind fails, as if memory ran out, until it is
// set back.
inline void setFailing(Job job, bool fail)
{
    if (fail) failing.fetch_or(unsigned(job));
    else failing.fetch_and(~unsigned(job));
}

// Called by a job as it starts: throws as a test asked.
inline void check(Job job)
{
    if (failing.load(std::memory_order_relaxed) & unsigned(job)) throw std::bad_alloc();
}

} // namespace WorkerFailure
