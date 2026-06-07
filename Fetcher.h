#pragma once
#include <QObject>
#include <QJsonObject>

class QNetworkAccessManager;

// Сбор данных Claude + Cursor целиком на C++ (без Python).
// Живёт в worker-QThread: refresh() выполняет синхронный сбор с таймаутами на
// каждый запрос, пишет ~/.cache/apiwidget/state.json и эмитит stateReady().
class Fetcher : public QObject {
    Q_OBJECT
public:
    explicit Fetcher(QObject *parent = nullptr);

    static QString statePath();
    static QJsonObject loadCache();      // прочитать state.json (или {})

public slots:
    void refresh();                      // вызывать в worker-потоке (через queued-сигнал)

signals:
    void started();
    void stateReady(const QJsonObject &state);

private:
    QJsonObject collect();               // полный проход: prev → fetch → merge → write
    QJsonObject fetchClaude();
    QJsonObject fetchCursor();

    QNetworkAccessManager *nam_ = nullptr;
    bool busy_ = false;
};
