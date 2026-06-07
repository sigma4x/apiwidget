#pragma once
#include <QWidget>
#include <QJsonObject>

class QLabel;
class QVBoxLayout;
class QTimer;
class QThread;
class Fetcher;

// Sticky-note widget: shows state from Fetcher (in worker thread) and cache.
class MainWindow : public QWidget {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

signals:
    void requestRefresh();      // → Fetcher::refresh (queued, worker thread)

protected:
    void mousePressEvent(QMouseEvent *e) override;     // drag the frameless window
    void contextMenuEvent(QContextMenuEvent *e) override;
    void closeEvent(QCloseEvent *e) override;

private:
    void buildUi();
    void applySettings();
    void render(const QJsonObject &state);             // redraw cards
    void updateFooter(const QJsonObject &state);       // "updated N min ago" from fetched_at
    void setIntervalSeconds(int seconds);
    void setOpacityPercent(int pct);
    void setAlwaysOnTop(bool on);
    void setAutostart(bool on);
    bool autostartEnabled() const;

    QWidget *root_ = nullptr;
    QVBoxLayout *cards_ = nullptr;
    QLabel *updated_ = nullptr;
    QTimer *timer_ = nullptr;
    QThread *thread_ = nullptr;
    Fetcher *fetcher_ = nullptr;
    int intervalSec_ = 60;
};
