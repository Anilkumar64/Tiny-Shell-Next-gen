#include "MainWindow.h"
#include "TshStyle.h"
#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QFont>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QSplashScreen>
#include <QTimer>
#include <cmath>

// ── Splash screen drawn entirely in code (no external image required) ──────
static QPixmap makeSplash(int w, int h) {
  QPixmap px(w, h);
  px.fill(Qt::transparent);
  QPainter p(&px);
  p.setRenderHint(QPainter::Antialiasing);

  // Background
  p.fillRect(px.rect(), QColor("#0f1117"));

  // Subtle grid lines
  p.setPen(QPen(QColor(40, 45, 62), 1));
  for (int x = 0; x < w; x += 40)
    p.drawLine(x, 0, x, h);
  for (int y = 0; y < h; y += 40)
    p.drawLine(0, y, w, y);

  // Glow circle
  QRadialGradient glow(w / 2, h / 2, 200);
  glow.setColorAt(0.0, QColor(0, 180, 216, 40));
  glow.setColorAt(1.0, Qt::transparent);
  p.fillRect(px.rect(), glow);

  // Hexagon logo
  const int cx = w / 2, cy = h / 2 - 30;
  const int R = 48;
  QPolygonF hex;
  for (int i = 0; i < 6; ++i) {
    const double ang = M_PI / 180.0 * (60.0 * i - 30.0);
    hex << QPointF(cx + R * std::cos(ang), cy + R * std::sin(ang));
  }
  p.setPen(QPen(QColor("#00b4d8"), 2.5));
  p.setBrush(QColor(0, 180, 216, 18));
  p.drawPolygon(hex);

  // Inner hex
  const int r2 = 30;
  QPolygonF hex2;
  for (int i = 0; i < 6; ++i) {
    const double ang = M_PI / 180.0 * (60.0 * i - 30.0);
    hex2 << QPointF(cx + r2 * std::cos(ang), cy + r2 * std::sin(ang));
  }
  p.setPen(QPen(QColor("#48cae4"), 1.5));
  p.setBrush(Qt::NoBrush);
  p.drawPolygon(hex2);

  // "T" glyph inside hex
  p.setPen(QPen(QColor("#00b4d8"), 3, Qt::SolidLine, Qt::RoundCap));
  p.drawLine(cx - 12, cy - 10, cx + 12, cy - 10);
  p.drawLine(cx, cy - 10, cx, cy + 14);

  // Product name
  QFont nameFont("Inter,Segoe UI,SF Pro Display,sans-serif", 22, QFont::Bold);
  p.setFont(nameFont);
  p.setPen(QColor("#e8eaf0"));
  QRect nameRect(0, cy + R + 16, w, 40);
  p.drawText(nameRect, Qt::AlignHCenter, "TinyShell NextGen");

  // Subtitle
  QFont subFont("Inter,Segoe UI,sans-serif", 11);
  p.setFont(subFont);
  p.setPen(QColor("#4a5068"));
  QRect subRect(0, cy + R + 56, w, 24);
  p.drawText(subRect, Qt::AlignHCenter,
             "Distributed Shell Orchestration Platform  ·  v3.0");

  // Loading bar background
  const int barW = 280, barH = 4;
  const int bx = (w - barW) / 2, by = h - 50;
  p.setPen(Qt::NoPen);
  p.setBrush(QColor("#1c1f2b"));
  p.drawRoundedRect(bx, by, barW, barH, 2, 2);

  // Loading bar fill (60% placeholder)
  QLinearGradient barGrad(bx, 0, bx + barW, 0);
  barGrad.setColorAt(0.0, QColor("#0077a8"));
  barGrad.setColorAt(1.0, QColor("#00b4d8"));
  p.setBrush(barGrad);
  p.drawRoundedRect(bx, by, int(barW * 0.65), barH, 2, 2);

  p.setPen(QColor("#4a5068"));
  QFont statusFont("Inter,Segoe UI,mono", 10);
  p.setFont(statusFont);
  p.drawText(QRect(0, h - 36, w, 20), Qt::AlignHCenter, "Initializing…");

  p.end();
  return px;
}

int main(int argc, char *argv[]) {
  // High-DPI scaling
  QApplication::setHighDpiScaleFactorRoundingPolicy(
      Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

  QApplication app(argc, argv);
  app.setApplicationName("TinyShell NextGen");
  app.setApplicationVersion("3.0.0");
  app.setOrganizationName("TinyShell");
  app.setOrganizationDomain("tinyshell.local");

  // ── CLI arguments ──────────────────────────────────────────────────────
  QCommandLineParser cli;
  cli.setApplicationDescription(
      "TinyShell NextGen GUI — Distributed Shell Control Panel");
  cli.addHelpOption();
  cli.addVersionOption();

  QCommandLineOption urlOpt({"u", "url"}, "API base URL", "url",
                            "https://127.0.0.1:8080");
  QCommandLineOption tokenOpt({"t", "token"}, "API bearer token", "token", "");
  cli.addOption(urlOpt);
  cli.addOption(tokenOpt);
  cli.process(app);

  // ── Splash ────────────────────────────────────────────────────────────
  const QSize splashSize(480, 300);
  QSplashScreen splash(makeSplash(splashSize.width(), splashSize.height()),
                       Qt::WindowStaysOnTopHint);
  splash.show();
  app.processEvents();

  // ── Main window ───────────────────────────────────────────────────────
  // IMPORTANT: pass url and token into the constructor so m_api is
  // configured before buildUi() fires the first polling requests.
  // Calling setApiUrl/setApiToken *after* construction was the root
  // cause of the "Bearer token rejected" burst (Bug B).
  auto *win = new MainWindow(nullptr, cli.value(urlOpt), cli.value(tokenOpt));

  // Center on screen
  if (auto *screen = QGuiApplication::primaryScreen()) {
    const QRect sg = screen->availableGeometry();
    win->move((sg.width() - win->width()) / 2,
              (sg.height() - win->height()) / 2);
  }

  // Dismiss splash after 1.8s, then show main window
  QTimer::singleShot(1800, &splash, [&splash, win] {
    win->show();
    splash.finish(win);
  });

  return app.exec();
}