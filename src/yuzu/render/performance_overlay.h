// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <deque>
#include <optional>
#include <QFont>
#include <QWidget>

#include "system_monitor.h"

namespace VideoCore {
class ShaderNotify;
}
namespace Core {
struct PerfStatsResults;
}

class MainWindow;

class PerformanceOverlay : public QWidget {
    Q_OBJECT

public:
    explicit PerformanceOverlay(MainWindow* parent = nullptr);
    ~PerformanceOverlay();

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    struct Meter {
        QString label;
        QString value;
        QString unit;
        std::optional<double> fraction;
        QColor accent;
    };

    void resetPosition(const QPoint& pos);
    void updateStats(const Core::PerfStatsResults& results, const VideoCore::ShaderNotify& shaders);

    void drawFpsGraph(QPainter& p, const QRectF& r) const;
    void drawMeter(QPainter& p, const QRectF& r, const Meter& meter) const;
    qreal drawValue(QPainter& p, qreal right, qreal baseline, const QString& value,
                    const QFont& value_font, const QColor& value_color, const QString& unit) const;
    void drawText(QPainter& p, const QPointF& pos, const QString& text, const QFont& font,
                  const QColor& color) const;

    MainWindow* m_mainWindow = nullptr;

    QPoint m_offset{25, 75};
    QPoint m_drag_start_pos;

    QFont m_labelFont;
    QFont m_fpsFont;
    QFont m_metricFont;
    QFont m_unitFont;

    static constexpr size_t NUM_FPS_SAMPLES = 120;
    std::deque<double> m_fpsSamples;
    std::optional<double> m_fps;
    std::optional<double> m_frametime;

    SystemMonitor m_monitor;
    SystemMonitor::Snapshot m_system;

signals:
    void closed();
};
