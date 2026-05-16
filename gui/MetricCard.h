#pragma once
#include "TshStyle.h"
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPropertyAnimation>
#include <QTimer>
#include <QVBoxLayout>

// A polished card widget showing a single KPI with icon, label and value.
class MetricCard : public QFrame {
  Q_OBJECT
public:
  enum Variant { Default, Success, Warning, Danger };

  explicit MetricCard(const QString &title, const QString &icon,
                      Variant variant = Default, QWidget *parent = nullptr)
      : QFrame(parent), m_variant(variant) {
    setObjectName("MetricCard");
    setFixedHeight(110);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    applyCardStyle();

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 14, 18, 14);
    root->setSpacing(8);

    // Top row: icon + title
    auto *topRow = new QHBoxLayout;
    topRow->setSpacing(10);

    m_iconLbl = new QLabel(icon, this);
    m_iconLbl->setFixedSize(28, 28);
    m_iconLbl->setAlignment(Qt::AlignCenter);
    m_iconLbl->setStyleSheet(QString("font-size: 16px;"
                                     "background: %1;"
                                     "border-radius: 8px;"
                                     "color: %2;")
                                 .arg(accentBg(), accentFg()));

    m_titleLbl = new QLabel(title.toUpper(), this);
    m_titleLbl->setStyleSheet(
        "font-size: 10px; font-weight: 600; letter-spacing: 0.8px;"
        "color: " +
        TshStyle::TEXT_SECONDARY.name() + ";");

    topRow->addWidget(m_iconLbl);
    topRow->addWidget(m_titleLbl);
    topRow->addStretch();

    // Value + subtext
    m_valueLbl = new QLabel("—", this);
    m_valueLbl->setStyleSheet(
        QString("font-size: 28px; font-weight: 700; color: %1;")
            .arg(accentFg()));

    m_subLbl = new QLabel("", this);
    m_subLbl->setStyleSheet(
        "font-size: 11px; color: " + TshStyle::TEXT_MUTED.name() + ";");

    root->addLayout(topRow);
    root->addWidget(m_valueLbl);
    root->addWidget(m_subLbl);
    root->addStretch();
  }

  void setValue(const QString &v) { m_valueLbl->setText(v); }
  void setSub(const QString &s) { m_subLbl->setText(s); }
  void setIcon(const QString &i) { m_iconLbl->setText(i); }

  void pulse() {
    // Briefly flash accent border on update
    setStyleSheet(objectName().isEmpty()
                      ? ""
                      : frameStyleSheet() + "border-color: " + accentFg() +
                            ";");
    QTimer::singleShot(400, this, [this] { applyCardStyle(); });
  }

private:
  void applyCardStyle() { setStyleSheet(frameStyleSheet()); }
  QString frameStyleSheet() const {
    return QString("QFrame#MetricCard {"
                   "  background-color: #1c1f2b;"
                   "  border: 1px solid #2a2d3e;"
                   "  border-radius: 10px;"
                   "}");
  }
  QString accentBg() const {
    switch (m_variant) {
    case Success:
      return "#0d2e1a";
    case Warning:
      return "#2e2208";
    case Danger:
      return "#2e0d0d";
    default:
      return "#0d1e2e";
    }
  }
  QString accentFg() const {
    switch (m_variant) {
    case Success:
      return TshStyle::SUCCESS.name();
    case Warning:
      return TshStyle::WARNING.name();
    case Danger:
      return TshStyle::DANGER.name();
    default:
      return TshStyle::ACCENT.name();
    }
  }

  QLabel *m_iconLbl;
  QLabel *m_titleLbl;
  QLabel *m_valueLbl;
  QLabel *m_subLbl;
  Variant m_variant;
};
