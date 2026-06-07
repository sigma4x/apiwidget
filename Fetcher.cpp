#include "Fetcher.h"
#include "usage_logic.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QThread>
#include <QFile>
#include <QSaveFile>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSet>
#include <QMap>
#include <QStringList>
#include <QUrl>

namespace {

using namespace usagelogic;   // jint/nowIso/isoFromMs/parseIso/msgTokens/windowBounds/merge* — в usage_logic.h

// ---------- HTTP (синхронно, с таймаутом и ретраем на 429) ----------

bool httpGetJson(QNetworkAccessManager *nam, const QString &url,
                 const QMap<QString, QString> &headers, const QByteArray &verb,
                 const QByteArray &body, QJsonDocument &outDoc, QString &err,
                 int retries429 = 0) {
    for (int attempt = 0;; ++attempt) {
        QNetworkRequest req((QUrl(url)));
        req.setRawHeader("Accept-Encoding", "identity");
        for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
            req.setRawHeader(it.key().toUtf8(), it.value().toUtf8());

        QNetworkReply *reply = (verb == "GET") ? nam->get(req)
                                               : nam->sendCustomRequest(req, verb, body);

        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        bool timedOut = false;
        QObject::connect(&timer, &QTimer::timeout, &loop, [&] { timedOut = true; reply->abort(); loop.quit(); });
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        timer.start(20000);                    // P3: жёсткий таймаут на каждый запрос
        loop.exec();
        timer.stop();

        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray payload = reply->readAll();
        const QByteArray retryAfter = reply->rawHeader("Retry-After");
        const QString netErr = timedOut ? QStringLiteral("timeout")
                             : (reply->error() != QNetworkReply::NoError ? reply->errorString() : QString());
        reply->deleteLater();

        if (code == 429 && attempt < retries429) {
            int wait = 30;
            bool ok = false;
            const int v = QString::fromLatin1(retryAfter).trimmed().toInt(&ok);
            if (ok && v > 0) wait = qMin(v, 30);
            QThread::sleep(wait);
            continue;
        }
        if (code >= 200 && code < 300 && netErr.isEmpty()) {
            QJsonParseError pe;
            outDoc = QJsonDocument::fromJson(payload, &pe);
            if (pe.error != QJsonParseError::NoError) { err = QStringLiteral("bad json"); return false; }
            return true;
        }
        err = !netErr.isEmpty() ? netErr : QStringLiteral("HTTP %1").arg(code);
        return false;
    }
}

// ---------- локальные учётки ----------

QJsonObject claudeOauth(QString &err) {
    const QString p = QDir::homePath() + "/.claude/.credentials.json";
    QFile f(p);
    if (!f.exists()) { err = QStringLiteral("Claude: не залогинен (нет .credentials.json)"); return {}; }
    if (!f.open(QIODevice::ReadOnly)) { err = QStringLiteral("Claude: не читается .credentials.json"); return {}; }
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object().value("claudeAiOauth").toObject();
    if (o.value("accessToken").toString().isEmpty() && o.value("access_token").toString().isEmpty())
        err = QStringLiteral("Claude: нет OAuth-токена");
    return o;
}

struct CursorCreds { QString accessToken, sub, plan, status, err; };

CursorCreds cursorCreds() {
    CursorCreds c;
    const QString src = QDir::homePath() + "/.config/Cursor/User/globalStorage/state.vscdb";
    if (!QFile::exists(src)) { c.err = QStringLiteral("Cursor: не залогинен (нет state.vscdb)"); return c; }

    // копия (+wal/shm) во временную папку — чтобы не драться за блокировку с Cursor
    QTemporaryDir tmp;
    if (!tmp.isValid()) { c.err = QStringLiteral("Cursor: нет временной папки"); return c; }
    const QString dst = tmp.path() + "/state.vscdb";
    QFile::copy(src, dst);
    QFile::copy(src + "-wal", dst + "-wal");
    QFile::copy(src + "-shm", dst + "-shm");

    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "apiwidget_cursor");
        db.setDatabaseName(dst);   // читаем одноразовую temp-копию, блокировок нет
        if (db.open()) {
            auto get = [&](const QString &k) -> QString {
                QSqlQuery q(db);
                q.prepare("SELECT value FROM ItemTable WHERE key = ?");
                q.addBindValue(k);
                if (q.exec() && q.next()) {
                    QString v = q.value(0).toString();       // значения — JSON-строки ("...")
                    if (v.size() >= 2 && v.startsWith('"') && v.endsWith('"'))
                        v = v.mid(1, v.size() - 2);
                    return v;
                }
                return {};
            };
            c.accessToken = get("cursorAuth/accessToken");
            c.plan = get("cursorAuth/stripeMembershipType");
            c.status = get("cursorAuth/stripeSubscriptionStatus");
        } else {
            c.err = QStringLiteral("Cursor: не открыть state.vscdb");
        }
    }
    QSqlDatabase::removeDatabase("apiwidget_cursor");
    if (!c.err.isEmpty()) return c;

    if (c.accessToken.isEmpty()) { c.err = QStringLiteral("Cursor: не залогинен (нет токена)"); return c; }
    const QStringList parts = c.accessToken.split('.');
    if (parts.size() < 2) { c.err = QStringLiteral("Cursor: повреждён токен"); return c; }
    const QByteArray payload = QByteArray::fromBase64(parts[1].toUtf8(), QByteArray::Base64UrlEncoding);
    c.sub = QJsonDocument::fromJson(payload).object().value("sub").toString();
    if (c.sub.isEmpty()) { c.err = QStringLiteral("Cursor: нет sub в токене"); return c; }
    return c;
}

// ---------- Claude: локальные токены в ОГРАНИЧЕННОМ окне [start, end] (P4) ----------

qint64 claudeLocalTokens(const QDateTime &start, const QDateTime &end) {
    qint64 total = 0;
    const QString rootDir = QDir::homePath() + "/.claude/projects";
    QDirIterator it(rootDir, QStringList{"*.jsonl"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        if (fi.lastModified().toUTC() < start) continue;     // файл не мог содержать сообщений окна
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        while (!f.atEnd()) {
            QJsonParseError pe;
            const QJsonObject o = QJsonDocument::fromJson(f.readLine(), &pe).object();
            if (pe.error != QJsonParseError::NoError || o.isEmpty()) continue;
            const QDateTime t = parseIso(o.value("timestamp").toString());
            if (!t.isValid() || t < start || t > end) continue;   // верхняя граница — фикс P4
            const QJsonObject usage = o.value("message").toObject().value("usage").toObject();
            if (!usage.isEmpty()) total += msgTokens(usage);
        }
    }
    return total;
}

// окно квоты Claude: {used_pct, resets_at, tokens, tokens_source}; границы — windowBounds (P4)
QJsonValue claudeWindow(const QJsonObject &o, int hours, bool fallback) {
    if (!fallback && o.isEmpty()) return QJsonValue();     // напр. seven_day_opus = null
    const QString resetsStr = fallback ? QString() : o.value("resets_at").toString();
    const auto [start, end] = windowBounds(resetsStr, hours, fallback, QDateTime::currentDateTimeUtc());
    return QJsonObject{
        {"used_pct", fallback ? QJsonValue() : o.value("utilization")},
        {"resets_at", fallback ? QJsonValue() : o.value("resets_at")},
        {"tokens", double(claudeLocalTokens(start, end))}, {"tokens_source", "local_jsonl"},
    };
}

} // namespace

// ---------- класс ----------

Fetcher::Fetcher(QObject *parent) : QObject(parent) {
    qRegisterMetaType<QJsonObject>("QJsonObject");         // для queued-сигнала stateReady
}

QString Fetcher::statePath() { return QDir::homePath() + "/.cache/apiwidget/state.json"; }

QJsonObject Fetcher::loadCache() {
    QFile f(statePath());
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

void Fetcher::refresh() {
    if (busy_) return;                                     // нет наложения запусков (P3)
    busy_ = true;
    emit started();
    if (!nam_) nam_ = new QNetworkAccessManager(this);     // создаётся в worker-потоке
    const QJsonObject st = collect();
    emit stateReady(st);
    busy_ = false;
}

QJsonObject Fetcher::collect() {
    const QJsonObject prev = loadCache();
    QJsonObject state;
    state["generated_at"] = nowIso();
    state["claude"] = mergeClaude(prev.value("claude").toObject(), fetchClaude());
    state["cursor"] = mergeCursor(prev.value("cursor").toObject(), fetchCursor());

    QDir().mkpath(QFileInfo(statePath()).absolutePath());
    QSaveFile out(statePath());
    if (out.open(QIODevice::WriteOnly)) {
        out.write(QJsonDocument(state).toJson(QJsonDocument::Indented));
        out.commit();
        QFile::setPermissions(statePath(), QFile::ReadOwner | QFile::WriteOwner);
    }
    return state;
}

QJsonObject Fetcher::fetchClaude() {
    QString cerr;
    const QJsonObject oauth = claudeOauth(cerr);
    QJsonObject out;
    if (oauth.contains("subscriptionType")) out["plan"] = oauth.value("subscriptionType");
    out["invoice_date"] = QJsonValue();
    if (!cerr.isEmpty()) { out["last_error"] = cerr; out["cred_error"] = true; return out; }

    QString tok = oauth.value("accessToken").toString();
    if (tok.isEmpty()) tok = oauth.value("access_token").toString();
    const QMap<QString, QString> h{
        {"Authorization", "Bearer " + tok}, {"anthropic-beta", "oauth-2025-04-20"},
        {"anthropic-version", "2023-06-01"}, {"User-Agent", "claude-code/2.0.32"},
        {"Accept", "application/json"},
    };

    QJsonDocument doc;
    QString err;
    if (!httpGetJson(nam_, "https://api.anthropic.com/api/oauth/usage", h, "GET", {}, doc, err, 1)) {
        out["last_error"] = "usage: " + err;
        out["api_error"] = true;
        out["session"] = claudeWindow({}, 5, true);        // токены окна посчитаем локально
        out["weekly_all"] = claudeWindow({}, 24 * 7, true);
        return out;
    }
    const QJsonObject b = doc.object();
    out["session"] = claudeWindow(b.value("five_hour").toObject(), 5, false);
    out["weekly_all"] = claudeWindow(b.value("seven_day").toObject(), 24 * 7, false);
    out["weekly_opus"] = claudeWindow(b.value("seven_day_opus").toObject(), 24 * 7, false);
    out["weekly_sonnet"] = claudeWindow(b.value("seven_day_sonnet").toObject(), 24 * 7, false);
    out["extra_usage_enabled"] = b.value("extra_usage").toObject().value("is_enabled");
    out["ok"] = true;
    return out;
}

QJsonObject Fetcher::fetchCursor() {
    const CursorCreds c = cursorCreds();
    QJsonObject out;
    if (!c.plan.isEmpty()) out["plan"] = c.plan;
    if (!c.status.isEmpty()) out["plan_status"] = c.status;
    if (!c.err.isEmpty()) { out["last_error"] = c.err; out["cred_error"] = true; return out; }

    const QString cookie = "WorkosCursorSessionToken=" + c.sub + "%3A%3A" + c.accessToken;
    const QMap<QString, QString> getH{{"Cookie", cookie}};
    const QMap<QString, QString> postH{
        {"Cookie", cookie}, {"Origin", "https://cursor.com"},
        {"Referer", "https://cursor.com/dashboard"}, {"Content-Type", "application/json"},
    };
    QStringList errs;

    {   // план / статус
        QJsonDocument d; QString e;
        if (httpGetJson(nam_, "https://cursor.com/api/auth/stripe", getH, "GET", {}, d, e)) {
            const QJsonObject s = d.object();
            out["plan"] = s.value("membershipType");
            out["plan_status"] = s.value("subscriptionStatus");
            out["is_yearly"] = s.value("isYearlyPlan");
        } else errs << "stripe: " + e;
    }
    {   // конец оплаченного периода
        QJsonDocument d; QString e;
        if (httpGetJson(nam_, "https://cursor.com/api/dashboard/get-current-billing-cycle", postH, "POST", "{}", d, e)) {
            const QJsonObject cy = d.object();
            out["invoice_date"] = isoFromMs(cy.value("endDateEpochMillis"));
            out["billing_cycle_start"] = isoFromMs(cy.value("startDateEpochMillis"));
        } else errs << "billing-cycle: " + e;
    }

    QJsonValue autoPct, apiPct;
    QSet<QString> autoModels;
    {   // included usage: % + разбиение auto/api
        QJsonDocument d; QString e;
        if (httpGetJson(nam_, "https://cursor.com/api/dashboard/get-current-period-usage", postH, "POST", "{}", d, e)) {
            const QJsonObject pu = d.object().value("planUsage").toObject();
            autoPct = pu.value("autoPercentUsed");
            apiPct = pu.value("apiPercentUsed");
            for (const QJsonValue &m : d.object().value("autoBucketModels").toArray()) autoModels.insert(m.toString());
        } else errs << "period-usage: " + e;
    }
    {   // токены по моделям
        QJsonDocument d; QString e;
        if (httpGetJson(nam_, "https://cursor.com/api/dashboard/get-aggregated-usage-events", postH, "POST", "{}", d, e)) {
            qint64 autoT = 0, apiT = 0;
            QJsonArray autoArr, apiArr;
            for (const QJsonValue &av : d.object().value("aggregations").toArray()) {
                const QJsonObject a = av.toObject();
                const QString model = a.value("modelIntent").toString();
                const qint64 t = jint(a.value("inputTokens")) + jint(a.value("outputTokens")) + jint(a.value("cacheReadTokens"));
                const QJsonObject mm{{"model", model}, {"tokens", double(t)},
                                     {"cost_usd", qRound(a.value("totalCents").toDouble()) / 100.0}};
                if (autoModels.contains(model)) { autoT += t; autoArr.append(mm); }
                else { apiT += t; apiArr.append(mm); }
            }
            out["auto_composer"] = QJsonObject{{"total_tokens", double(autoT)}, {"used_pct", autoPct}, {"by_model", autoArr}, {"source_fresh", true}};
            out["api_tokens"] = QJsonObject{{"tokens", double(apiT)}, {"used_pct", apiPct}, {"by_model", apiArr}, {"source_fresh", true}};
        } else errs << "agg: " + e;
    }

    if (!errs.isEmpty()) out["last_error"] = errs.join(" | ");
    return out;
}
