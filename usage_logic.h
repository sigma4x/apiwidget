#pragma once
// Чистая логика fetcher'а (без сети/sqlite/файлов) — чтобы покрыть тестами.
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QDateTime>
#include <QTimeZone>
#include <QPair>

namespace usagelogic {

inline qint64 jint(const QJsonValue &v) {            // токены приходят и числом, и строкой
    return v.isString() ? v.toString().toLongLong() : static_cast<qint64>(v.toDouble());
}

inline QString nowIso() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }

inline QJsonValue isoFromMs(const QJsonValue &v) {
    const qint64 ms = jint(v);
    if (ms <= 0) return QJsonValue();
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(Qt::ISODate);
}

inline QDateTime parseIso(const QString &s) {
    if (s.isEmpty()) return {};
    QDateTime t = QDateTime::fromString(s, Qt::ISODateWithMs);
    if (!t.isValid()) t = QDateTime::fromString(s, Qt::ISODate);
    return t.isValid() ? t.toUTC() : QDateTime();
}

inline qint64 msgTokens(const QJsonObject &u) {
    return jint(u.value("input_tokens")) + jint(u.value("output_tokens"))
         + jint(u.value("cache_creation_input_tokens")) + jint(u.value("cache_read_input_tokens"));
}

// Границы окна [start, end]. Верхняя граница = resets_at (или now) — это фикс P4:
// окно не «растёт» вверх при устаревшем resets_at.
inline QPair<QDateTime, QDateTime> windowBounds(const QString &resetsIso, int hours,
                                                bool fallback, const QDateTime &now) {
    QDateTime end = fallback ? now : parseIso(resetsIso);
    if (!end.isValid()) end = now;
    return {end.addSecs(-qint64(hours) * 3600), end};
}

// При 429 сохраняем % и resets_at из кэша, но локальные токени берём свежие.
inline QJsonValue mergeWindow(const QJsonValue &prevV, const QJsonValue &newV) {
    const QJsonObject p = prevV.toObject(), n = newV.toObject();
    if (n.isEmpty()) return prevV;
    if (p.isEmpty()) return newV;
    QJsonObject m = p;
    for (const QString &k : n.keys())
        if (!n.value(k).isNull()) m[k] = n.value(k);
    if (n.value("used_pct").isNull() && !p.value("used_pct").isNull()) m["used_pct"] = p.value("used_pct");
    if (n.value("resets_at").toString().isEmpty() && !p.value("resets_at").toString().isEmpty())
        m["resets_at"] = p.value("resets_at");
    return m;
}

// Не затираем last-good при ошибке usage; fetched_at НЕ обновляем при stale (P1).
inline QJsonObject mergeClaude(const QJsonObject &prev, const QJsonObject &fresh) {
    QJsonObject out;
    out["plan"] = (fresh.contains("plan") && !fresh.value("plan").isNull()) ? fresh.value("plan") : prev.value("plan");
    out["invoice_date"] = prev.value("invoice_date");
    if (fresh.value("ok").toBool()) {
        for (const char *k : {"session", "weekly_all", "weekly_opus", "weekly_sonnet", "extra_usage_enabled"})
            out[k] = fresh.value(k);
        out["fetched_at"] = nowIso();
        out["stale"] = false;
        out["last_error"] = QJsonValue();
    } else {
        out["session"] = mergeWindow(prev.value("session"), fresh.value("session"));
        out["weekly_all"] = mergeWindow(prev.value("weekly_all"), fresh.value("weekly_all"));
        out["weekly_opus"] = prev.value("weekly_opus");
        out["weekly_sonnet"] = prev.value("weekly_sonnet");
        out["extra_usage_enabled"] = prev.value("extra_usage_enabled");
        out["fetched_at"] = prev.value("fetched_at");
        out["stale"] = true;
        out["last_error"] = fresh.value("last_error");
    }
    return out;
}

inline QJsonObject markCached(const QJsonValue &secV) {
    QJsonObject s = secV.toObject();
    if (!s.isEmpty()) s["source_fresh"] = false;
    return s;
}

// Частичный отказ Cursor (P2): свежие секции — as is, отсутствующие — из кэша c source_fresh=false.
inline QJsonObject mergeCursor(const QJsonObject &prev, const QJsonObject &fresh) {
    QJsonObject out = prev;
    for (const char *k : {"plan", "plan_status", "is_yearly", "invoice_date", "billing_cycle_start"})
        if (fresh.contains(k) && !fresh.value(k).isNull()) out[k] = fresh.value(k);
    for (const char *k : {"auto_composer", "api_tokens"}) {
        if (fresh.contains(k)) out[k] = fresh.value(k);
        else out[k] = markCached(prev.value(k));
    }
    const bool quotaFresh = fresh.contains("auto_composer") && fresh.contains("api_tokens");
    const QString err = fresh.value("last_error").toString();
    out["last_error"] = err.isEmpty() ? QJsonValue() : QJsonValue(err);
    out["stale"] = !(err.isEmpty() && !fresh.value("cred_error").toBool() && quotaFresh);
    if (quotaFresh) out["fetched_at"] = nowIso();
    return out;
}

} // namespace usagelogic
