#include <QtTest>
#include "../usage_logic.h"

using namespace usagelogic;

// Регрессионные тесты на чистую логику fetcher'а. Без сети, без creds, без файлов.
// Запуск: ctest --test-dir build   (или ./build/test_usage_logic)
class TestUsageLogic : public QObject {
    Q_OBJECT

private:
    static QDateTime utc(const QString &iso) {
        return QDateTime::fromString(iso, Qt::ISODate).toUTC();
    }

private slots:
    void jint_parsesStringAndNumber() {
        QCOMPARE(jint(QJsonValue(QStringLiteral("2701879"))), qint64(2701879));
        QCOMPARE(jint(QJsonValue(45.0)), qint64(45));
        QCOMPARE(jint(QJsonValue()), qint64(0));
    }

    // P4: верхняя граница окна = resets_at, а не «сейчас».
    void windowBounds_upperBoundIsReset() {
        const QDateTime now = utc("2026-06-07T10:00:00Z");
        const auto [start, end] = windowBounds("2026-06-07T13:40:00Z", 5, false, now);
        QCOMPARE(end, utc("2026-06-07T13:40:00Z"));
        QCOMPARE(start, end.addSecs(-5 * 3600));
    }

    // P4: при устаревшем resets_at окно не растёт до now.
    void windowBounds_staleResetStaysBounded() {
        const QDateTime now = utc("2026-06-07T10:00:00Z");
        const auto [start, end] = windowBounds("2026-06-01T00:00:00Z", 5, false, now);
        QVERIFY(end < now);
        QCOMPARE(end, utc("2026-06-01T00:00:00Z"));
        QCOMPARE(start, utc("2026-05-31T19:00:00Z"));
    }

    void windowBounds_fallbackUsesNow() {
        const QDateTime now = utc("2026-06-07T10:00:00Z");
        const auto [start, end] = windowBounds(QString(), 5, true, now);
        QCOMPARE(end, now);
        QCOMPARE(start, now.addSecs(-5 * 3600));
    }

    // 429: % и resets_at из кэша, токены — свежие.
    void mergeWindow_keepsPrevPctTakesFreshTokens() {
        const QJsonObject prev{{"used_pct", 34}, {"resets_at", "2026-06-07T13:00:00Z"}, {"tokens", 100.0}};
        const QJsonObject neu{{"used_pct", QJsonValue()}, {"resets_at", ""}, {"tokens", 250.0}};
        const QJsonObject m = mergeWindow(prev, neu).toObject();
        QCOMPARE(m.value("used_pct").toInt(), 34);
        QCOMPARE(m.value("resets_at").toString(), QStringLiteral("2026-06-07T13:00:00Z"));
        QCOMPARE(qint64(m.value("tokens").toDouble()), qint64(250));
    }

    void mergeClaude_freshSetsFetchedAndNotStale() {
        const QJsonObject prev{{"fetched_at", "2026-06-07T09:00:00Z"},
                               {"session", QJsonObject{{"used_pct", 10}}}};
        const QJsonObject fresh{{"ok", true}, {"plan", "max"},
                                {"session", QJsonObject{{"used_pct", 30}}},
                                {"weekly_all", QJsonObject{}}, {"weekly_opus", QJsonValue()},
                                {"weekly_sonnet", QJsonValue()}, {"extra_usage_enabled", false}};
        const QJsonObject m = mergeClaude(prev, fresh);
        QCOMPARE(m.value("stale").toBool(), false);
        QCOMPARE(m.value("plan").toString(), QStringLiteral("max"));
        QCOMPARE(m.value("session").toObject().value("used_pct").toInt(), 30);
        QVERIFY(!m.value("fetched_at").toString().isEmpty());
        QVERIFY(m.value("fetched_at").toString() != QStringLiteral("2026-06-07T09:00:00Z"));
    }

    // P1: при ошибке usage держим last-good, fetched_at НЕ обновляем.
    void mergeClaude_apiErrorKeepsPrevAndStale() {
        const QJsonObject prev{{"fetched_at", "2026-06-07T09:00:00Z"}, {"plan", "max"},
                               {"session", QJsonObject{{"used_pct", 10},
                                                       {"resets_at", "2026-06-07T13:00:00Z"},
                                                       {"tokens", 5.0}}}};
        const QJsonObject fresh{{"api_error", true}, {"last_error", "usage: timeout"},
                                {"session", QJsonObject{{"used_pct", QJsonValue()},
                                                        {"resets_at", ""}, {"tokens", 7.0}}}};
        const QJsonObject m = mergeClaude(prev, fresh);
        QCOMPARE(m.value("stale").toBool(), true);
        QCOMPARE(m.value("fetched_at").toString(), QStringLiteral("2026-06-07T09:00:00Z"));
        QCOMPARE(m.value("session").toObject().value("used_pct").toInt(), 10);          // % из кэша
        QCOMPARE(qint64(m.value("session").toObject().value("tokens").toDouble()), qint64(7)); // токены свежие
        QCOMPARE(m.value("last_error").toString(), QStringLiteral("usage: timeout"));
    }

    // P2: упал один endpoint — его секция из кэша (source_fresh=false), другая свежая.
    void mergeCursor_partialMarksFailedSectionCached() {
        const QJsonObject prev{{"auto_composer", QJsonObject{{"used_pct", 7.0}, {"source_fresh", true}}},
                               {"api_tokens", QJsonObject{{"used_pct", 0.5}, {"source_fresh", true}}},
                               {"fetched_at", "2026-06-07T09:00:00Z"}};
        const QJsonObject fresh{{"plan", "pro_plus"},
                                {"auto_composer", QJsonObject{{"used_pct", 8.0}, {"source_fresh", true}}},
                                {"last_error", "agg: timeout"}};   // api_tokens НЕ пришёл
        const QJsonObject m = mergeCursor(prev, fresh);
        QCOMPARE(m.value("auto_composer").toObject().value("source_fresh").toBool(), true);
        QCOMPARE(m.value("api_tokens").toObject().value("source_fresh").toBool(), false);
        QCOMPARE(m.value("api_tokens").toObject().value("used_pct").toDouble(), 0.5);   // из кэша
        QCOMPARE(m.value("stale").toBool(), true);
    }

    void mergeCursor_fullFreshNotStale() {
        const QJsonObject fresh{{"plan", "pro_plus"},
                                {"auto_composer", QJsonObject{{"used_pct", 8.0}, {"source_fresh", true}}},
                                {"api_tokens", QJsonObject{{"used_pct", 0.7}, {"source_fresh", true}}}};
        const QJsonObject m = mergeCursor(QJsonObject{}, fresh);
        QCOMPARE(m.value("stale").toBool(), false);
        QVERIFY(!m.value("fetched_at").toString().isEmpty());
    }
};

QTEST_MAIN(TestUsageLogic)
#include "test_usage_logic.moc"
