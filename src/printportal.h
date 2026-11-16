#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QSizeF>
#include <QVariantMap>

class QDBusPendingCallWatcher;
class QTemporaryFile;

// The desktop's printing: its print dialog, where the printer and the paper
// are chosen, then the job itself. Abstract, so that tests stand in a
// printer that answers without a desktop and never reaches a real one.
// One question is open at a time; a newer one replaces it.
class Printer : public QObject {
    Q_OBJECT

public:
    explicit Printer(QObject *parent = nullptr) : QObject(parent) {}
    ~Printer() override = default;

    // Asks how to print a document of this title. prepared() follows with
    // the paper chosen and a token that print() passes back; nothing follows
    // when the person cancels; failed() when it cannot be asked.
    virtual void prepare(const QString &title) = 0;
    // Prints a PDF as prepared. printed() or failed() follows.
    virtual void print(const QString &title, const QByteArray &pdf, quint32 token) = 0;

    // The paper as the print dialog describes it (xdg-desktop-portal's
    // page setup): Width and Height in millimetres, the margins, Orientation.
    struct Page {
        QSizeF size;  // millimetres, upright; empty when the dialog did not say
        bool landscape = false;
        qreal margin = 0; // the widest of the margins it gives, in millimetres
    };
    static Page pageOf(const QVariantMap &setup);

signals:
    void prepared(quint32 token, const QVariantMap &pageSetup);
    void printed();
    void failed(const QString &message);
};

// Printing through xdg-desktop-portal's Print interface: PreparePrint shows
// the desktop's print dialog, and Print hands the PDF over as a file
// descriptor. Nothing here waits on the portal.
class PortalPrinter : public Printer {
    Q_OBJECT

public:
    explicit PortalPrinter(QObject *parent = nullptr);
    ~PortalPrinter() override;

    void prepare(const QString &title) override;
    void print(const QString &title, const QByteArray &pdf, quint32 token) override;

private slots:
    void handleResponse(uint response, const QVariantMap &results);

private:
    enum class Step { None, Prepare, Print };
    void request(const QString &method, const QList<QVariant> &arguments, const QString &token, Step step);
    void stopListening();
    void clear();

    Step m_step = Step::None;
    QString m_path;
    int m_generation = 0;
    QTemporaryFile *m_file = nullptr;
};
