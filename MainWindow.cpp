#include "MainWindow.h"
#include "Fetcher.h"
#include "desktop_integration.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QFrame>
#include <QSizeGrip>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QCloseEvent>
#include <QWindow>
#include <QThread>
#include <QTimer>
#include <QSettings>
#include <QFile>
#include <QDir>
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>

namespace {

// card row: label, value, sub-label, %, "cached" flag
struct Row {
    QString label;
    QString value;
    QString sub;
    double pct = -1.0;     // <0 → no progress bar
    bool cached = false;   // data from last-good cache
};

QString fmtTokens(qint64 n) {
    if (n >= 1000000) return QString::number(n / 1e6, 'f', 1) + "M";
    if (n >= 1000)    return QString::number(n / 1e3, 'f', 1) + "K";
    return QString::number(n);
}

QString resetsIn(const QString &iso) {
    if (iso.isEmpty()) return QString();
    QDateTime t = QDateTime::fromString(iso, Qt::ISODateWithMs);
    if (!t.isValid()) t = QDateTime::fromString(iso, Qt::ISODate);
    if (!t.isValid()) return QString();
    qint64 s = QDateTime::currentDateTimeUtc().secsTo(t.toUTC());
    if (s <= 0) return QStringLiteral("resets now");
    qint64 d = s / 86400; s %= 86400;
    qint64 h = s / 3600;  s %= 3600;
    qint64 m = s / 60;
    if (d > 0) return QString("resets in %1d %2h").arg(d).arg(h);
    if (h > 0) return QString("resets in %1h %2m").arg(h).arg(m);
    return QString("resets in %1m").arg(m);
}

QString pctStr(const QJsonObject &o) {
    double p = o.value("used_pct").toDouble(-1);
    return p < 0 ? QStringLiteral("—") : QString::number(p, 'f', 1) + "%";
}

qint64 quotaTokens(const QJsonObject &o) {
    if (o.contains(QStringLiteral("tokens")))
        return static_cast<qint64>(o.value(QStringLiteral("tokens")).toDouble(0));
    if (o.contains(QStringLiteral("total_tokens")))
        return static_cast<qint64>(o.value(QStringLiteral("total_tokens")).toDouble(0));
    return 0;
}

QString quotaValue(const QJsonObject &o) {
    const qint64 tk = quotaTokens(o);
    const QString pct = pctStr(o);
    if (pct != QStringLiteral("—"))
        return tk > 0 ? pct + QStringLiteral(" · ") + fmtTokens(tk) : pct;
    return tk > 0 ? fmtTokens(tk) : pct;
}

bool quotaVisible(const QJsonObject &o) {
    return o.value(QStringLiteral("used_pct")).toDouble(-1) > 0 || quotaTokens(o) > 0;
}

QString billingPeriod(const QJsonObject &cu) {
    const QString end = cu.value(QStringLiteral("invoice_date")).toString().left(10);
    const QString start = cu.value(QStringLiteral("billing_cycle_start")).toString().left(10);
    if (start.isEmpty() || end.isEmpty()) return QString();
    return start + QStringLiteral(" – ") + end;
}

QString barStyle(double pct) {
    QString c = pct >= 90 ? "#e5534b" : pct >= 70 ? "#d6a300" : "#2ea043";
    return QString(
        "QProgressBar{background:rgba(255,255,255,20);border:none;border-radius:3px;}"
        "QProgressBar::chunk{background:%1;border-radius:3px;}").arg(c);
}

QWidget *makeRow(const Row &r) {
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    v->setContentsMargins(0, 2, 0, 2);
    v->setSpacing(3);

    auto *top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    auto *lab = new QLabel(r.label);
    lab->setStyleSheet("color:#cfd2d6;");
    lab->setMinimumHeight(lab->fontMetrics().height());   // keep descenders (e.g. 'y') from clipping
    auto *val = new QLabel(r.value);
    val->setStyleSheet("color:#ffffff;font-weight:600;");
    val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    top->addWidget(lab);
    top->addStretch();
    top->addWidget(val);
    v->addLayout(top);

    if (r.pct >= 0) {
        auto *bar = new QProgressBar;
        bar->setRange(0, 100);
        bar->setValue(qBound(0, qRound(r.pct), 100));
        bar->setTextVisible(false);
        bar->setFixedHeight(6);
        bar->setStyleSheet(barStyle(r.pct));
        v->addWidget(bar);
    }

    QString sub = r.sub;
    if (r.cached) sub = sub.isEmpty() ? QStringLiteral("cached") : sub + QStringLiteral("  ·  cached");
    if (!sub.isEmpty()) {
        auto *s = new QLabel(sub);
        s->setStyleSheet(r.cached ? "color:#d6a300;font-size:11px;" : "color:#9aa0a6;font-size:11px;");
        v->addWidget(s);
    }
    return w;
}

QWidget *makeCard(const QString &title, const QString &meta,
                  const QList<Row> &rows, const QString &error) {
    auto *card = new QFrame;
    card->setObjectName("card");
    auto *v = new QVBoxLayout(card);
    v->setContentsMargins(12, 10, 12, 10);
    v->setSpacing(4);

    auto *th = new QHBoxLayout;
    auto *t = new QLabel(title);
    t->setStyleSheet("color:#ffffff;font-weight:700;font-size:14px;");
    auto *m = new QLabel(meta.isEmpty() ? QStringLiteral("—") : meta);
    m->setStyleSheet("color:#9aa0a6;font-size:11px;");
    m->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    th->addWidget(t);
    th->addStretch();
    th->addWidget(m);
    v->addLayout(th);

    for (const auto &r : rows) v->addWidget(makeRow(r));

    if (!error.isEmpty()) {
        auto *e = new QLabel("⚠ " + error);
        e->setWordWrap(true);
        e->setStyleSheet("color:#e5534b;font-size:11px;");
        v->addWidget(e);
    }
    return card;
}

} // namespace

MainWindow::MainWindow(QWidget *parent) : QWidget(parent) {
    setWindowTitle("Token Usage");
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setMinimumSize(220, 200);

    buildUi();

    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, [this] { Q_EMIT requestRefresh(); });

    // Fetcher lives in a separate thread: network/disk don't freeze the UI
    thread_ = new QThread(this);
    fetcher_ = new Fetcher;
    fetcher_->moveToThread(thread_);
    connect(thread_, &QThread::finished, fetcher_, &QObject::deleteLater);
    connect(this, &MainWindow::requestRefresh, fetcher_, &Fetcher::refresh);
    connect(fetcher_, &Fetcher::started, this, [this] { updated_->setText("updating…"); });
    connect(fetcher_, &Fetcher::stateReady, this, [this](const QJsonObject &s) { render(s); });
    thread_->start();

    applySettings();
    render(Fetcher::loadCache());     // show last-good cache instantly
    Q_EMIT requestRefresh();          // and refresh right away
    timer_->start();
}

MainWindow::~MainWindow() {
    thread_->quit();
    thread_->wait();
}

void MainWindow::buildUi() {
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    root_ = new QWidget;
    root_->setObjectName("root");
    outer->addWidget(root_);

    auto *main = new QVBoxLayout(root_);
    main->setContentsMargins(12, 10, 12, 8);
    main->setSpacing(8);

    auto *hdr = new QHBoxLayout;
    auto *title = new QLabel("Token Usage");
    title->setStyleSheet("color:#ffffff;font-weight:700;");
    auto *refresh = new QPushButton("⟳");
    refresh->setFixedSize(22, 22);
    refresh->setCursor(Qt::PointingHandCursor);
    refresh->setToolTip("Refresh now");
    refresh->setStyleSheet(
        "QPushButton{color:#cfd2d6;background:rgba(255,255,255,18);border:none;"
        "border-radius:11px;font-size:14px;}"
        "QPushButton:hover{background:rgba(255,255,255,38);}");
    connect(refresh, &QPushButton::clicked, this, [this] { Q_EMIT requestRefresh(); });
    hdr->addWidget(title);
    hdr->addStretch();
    hdr->addWidget(refresh);
    main->addLayout(hdr);

    auto *content = new QWidget;
    cards_ = new QVBoxLayout(content);
    cards_->setContentsMargins(0, 0, 0, 0);
    cards_->setSpacing(8);
    main->addWidget(content);
    main->addStretch();

    auto *foot = new QHBoxLayout;
    updated_ = new QLabel("—");
    updated_->setStyleSheet("color:#9aa0a6;font-size:11px;");
    foot->addWidget(updated_);
    foot->addStretch();
    foot->addWidget(new QSizeGrip(this), 0, Qt::AlignBottom | Qt::AlignRight);
    main->addLayout(foot);

    // #root/#card background is set by setOpacityPercent() (called from applySettings,
    // single source so the alpha applies; a second setStyleSheet here would not repaint).
}

void MainWindow::render(const QJsonObject &root) {
    while (QLayoutItem *it = cards_->takeAt(0)) {
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }

    // ---- Claude ----
    {
        const QJsonObject c = root.value("claude").toObject();
        const bool cached = c.value("stale").toBool();
        QList<Row> rows;
        // Claude's primary windows (Session/Weekly) stay visible even at 0%: right
        // after a usage reset utilization is legitimately 0 — hiding it emptied the card.
        auto add = [&](const QString &label, const QJsonObject &w) {
            if (w.isEmpty()) return;   // skip only a truly-absent window (null in the API)
            rows.push_back({label, quotaValue(w), resetsIn(w.value("resets_at").toString()),
                            w.value("used_pct").toDouble(-1), cached});
        };
        add("Session", c.value("session").toObject());
        add("Weekly · all", c.value("weekly_all").toObject());
        const QJsonObject sn = c.value("weekly_sonnet").toObject();
        if (sn.value("used_pct").toDouble(-1) > 0)
            rows.push_back({"Weekly · Sonnet", quotaValue(sn), resetsIn(sn.value("resets_at").toString()),
                            sn.value("used_pct").toDouble(-1), cached});
        cards_->addWidget(makeCard("Claude", c.value("plan").toString(), rows,
                                   c.value("last_error").toString()));
    }

    // ---- Cursor ----
    {
        const QJsonObject cu = root.value("cursor").toObject();
        QList<Row> rows;
        const QJsonObject ac = cu.value("auto_composer").toObject();
        if (quotaVisible(ac))
            rows.push_back({"Auto + Composer", quotaValue(ac), billingPeriod(cu),
                            ac.value("used_pct").toDouble(-1), !ac.value("source_fresh").toBool(true)});
        const QJsonObject api = cu.value("api_tokens").toObject();
        if (quotaVisible(api))
            rows.push_back({"API tokens", quotaValue(api), QString(),
                            api.value("used_pct").toDouble(-1), !api.value("source_fresh").toBool(true)});
        QString meta = cu.value("plan").toString();
        if (cu.value("invoice_date").isString())
            meta += "  ·  until " + cu.value("invoice_date").toString().left(10);
        cards_->addWidget(makeCard("Cursor", meta, rows, cu.value("last_error").toString()));
    }

    updateFooter(root);
}

void MainWindow::updateFooter(const QJsonObject &root) {
    QDateTime best;   // newest SUCCESSFUL fetched_at (P1: don't lie with file time)
    for (const char *p : {"claude", "cursor"}) {
        const QString fa = root.value(p).toObject().value("fetched_at").toString();
        QDateTime t = QDateTime::fromString(fa, Qt::ISODateWithMs);
        if (!t.isValid()) t = QDateTime::fromString(fa, Qt::ISODate);
        if (t.isValid() && (!best.isValid() || t > best)) best = t;
    }
    if (!best.isValid()) { updated_->setText("no fresh data"); return; }
    const qint64 mins = best.toUTC().secsTo(QDateTime::currentDateTimeUtc()) / 60;
    updated_->setText(mins <= 0 ? "updated just now"
                                : QString("updated %1 min ago").arg(mins));
}

void MainWindow::applySettings() {
    QSettings s;
    if (!restoreGeometry(s.value("geometry").toByteArray())) resize(300, 320);
    intervalSec_ = s.value("intervalSec", 60).toInt();
    timer_->setInterval(intervalSec_ * 1000);
    setOpacityPercent(s.value("opacity", 100).toInt());
    setAlwaysOnTop(s.value("alwaysOnTop", true).toBool());
}

void MainWindow::setIntervalSeconds(int sec) {
    intervalSec_ = sec;
    timer_->setInterval(sec * 1000);
    timer_->start();
    QSettings().setValue("intervalSec", sec);
}

void MainWindow::setOpacityPercent(int pct) {
    pct = qBound(20, pct, 100);
    const int a = qRound(236.0 * pct / 100.0);
    root_->setStyleSheet(QString(
        "#root{background:rgba(28,28,32,%1);border-radius:12px;}"
        "#card{background:rgba(255,255,255,14);border-radius:8px;}").arg(a));
    QSettings().setValue("opacity", pct);
}

void MainWindow::setAlwaysOnTop(bool on) {
    const bool wasVisible = isVisible();
    const QRect geo = geometry();
    setWindowFlag(Qt::WindowStaysOnTopHint, on);   // recreates the native window (withdraws it)
    QSettings().setValue("alwaysOnTop", on);
    if (wasVisible) {                              // re-map and restore place after recreation
        setGeometry(geo);
        show();
    }
}

bool MainWindow::autostartEnabled() const {
    return QFile::exists(QDir::homePath() + "/.config/autostart/apiwidget.desktop");
}

void MainWindow::setAutostart(bool on) {
    writeAutostartDesktop(on);
}

void MainWindow::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && windowHandle())
        windowHandle()->startSystemMove();   // works correctly under Wayland/X11
}

void MainWindow::contextMenuEvent(QContextMenuEvent *e) {
    QMenu menu(this);
    menu.addAction("Refresh now", this, [this] { Q_EMIT requestRefresh(); });

    auto *intMenu = menu.addMenu("Refresh interval");
    auto *grp = new QActionGroup(intMenu);
    struct { const char *label; int sec; } intervals[] = {
        {"1 min", 60}, {"5 min", 300}, {"10 min", 600}};
    for (const auto &it : intervals) {
        QAction *a = intMenu->addAction(it.label);
        a->setCheckable(true);
        a->setChecked(it.sec == intervalSec_);
        grp->addAction(a);
        connect(a, &QAction::triggered, this, [this, s = it.sec] { setIntervalSeconds(s); });
    }

    auto *opMenu = menu.addMenu("Opacity");
    const int curOp = QSettings().value("opacity", 100).toInt();
    for (int p : {100, 90, 80, 70, 60, 40}) {
        QAction *a = opMenu->addAction(QString("%1%").arg(p));
        a->setCheckable(true);
        a->setChecked(p == curOp);
        connect(a, &QAction::triggered, this, [this, p] { setOpacityPercent(p); });
    }

    QAction *top = menu.addAction("Always on top");
    top->setCheckable(true);
    const bool wasOnTop = QSettings().value("alwaysOnTop", true).toBool();
    top->setChecked(wasOnTop);

    QAction *au = menu.addAction("Start at login");
    au->setCheckable(true);
    au->setChecked(autostartEnabled());
    connect(au, &QAction::toggled, this, [this](bool on) { setAutostart(on); });

    menu.addSeparator();
    QAction *quitAct = menu.addAction("Quit");

    const QAction *chosen = menu.exec(e->globalPos());

    // Toggling on-top recreates the native window; doing it inside menu.exec()'s
    // nested loop (or this handler's stack) leaves it withdrawn/hidden. Defer to a
    // clean main-loop tick so the recreation+re-show happens safely.
    if (top->isChecked() != wasOnTop) {
        const bool on = top->isChecked();
        QTimer::singleShot(0, this, [this, on] { setAlwaysOnTop(on); });
    }
    if (chosen == quitAct)
        close();
}

void MainWindow::closeEvent(QCloseEvent *e) {
    QSettings().setValue("geometry", saveGeometry());
    e->accept();
    QCoreApplication::quit();
}
