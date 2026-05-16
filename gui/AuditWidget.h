#pragma once
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTableWidget>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QComboBox>
#include <QTimer>
#include <QDateTime>
#include <QRandomGenerator>
#include <QCheckBox>
#include "TshStyle.h"

// Audit log viewer — streams mock events; replace with ZkAuditTrail / StructuredAuditLogger
// API calls when backend exposes them.
class AuditWidget : public QWidget {
    Q_OBJECT

    struct AuditEntry {
        QString timestamp;
        QString severity;  // INFO | WARN | ERROR | CRITICAL
        QString user;
        QString action;
        QString resource;
        QString tenantId;
        QString outcome;   // SUCCESS | DENIED | FAILED
    };

public:
    explicit AuditWidget(QWidget* parent = nullptr) : QWidget(parent) {
        buildUi();
        seedEvents();
        m_ticker = new QTimer(this);
        connect(m_ticker, &QTimer::timeout, this, &AuditWidget::appendRandomEvent);
        m_ticker->start(4000);
    }

private:
    void buildUi() {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(28, 24, 28, 24);
        root->setSpacing(14);

        // ── Header ──────────────────────────────────────────────────────────
        auto* hdrRow = new QHBoxLayout;
        auto* titleLbl = new QLabel("Audit Log", this);
        titleLbl->setStyleSheet("font-size:22px; font-weight:700; color:#e8eaf0;");
        m_liveLbl = new QLabel("● Live", this);
        m_liveLbl->setStyleSheet("font-size:12px; color:#4ade80; font-weight:600;");
        auto* pauseBtn = new QPushButton("⏸ Pause", this);
        pauseBtn->setCheckable(true); pauseBtn->setFixedHeight(32);
        connect(pauseBtn, &QPushButton::toggled, this, [this,pauseBtn](bool p){
            p ? m_ticker->stop() : m_ticker->start(4000);
            pauseBtn->setText(p ? "▶ Resume" : "⏸ Pause");
            m_liveLbl->setStyleSheet(QString("font-size:12px; color:%1; font-weight:600;")
                .arg(p ? "#8891b0" : "#4ade80"));
            m_liveLbl->setText(p ? "● Paused" : "● Live");
        });
        auto* clearBtn = new QPushButton("✕ Clear", this);
        clearBtn->setFixedHeight(32);
        connect(clearBtn, &QPushButton::clicked, this, &AuditWidget::clearLog);
        auto* exportBtn = new QPushButton("⬇ Export", this);
        exportBtn->setFixedHeight(32);

        hdrRow->addWidget(titleLbl);
        hdrRow->addWidget(m_liveLbl);
        hdrRow->addStretch();
        hdrRow->addWidget(pauseBtn);
        hdrRow->addWidget(clearBtn);
        hdrRow->addWidget(exportBtn);
        root->addLayout(hdrRow);

        // ── Filter bar ───────────────────────────────────────────────────────
        auto* filterRow = new QHBoxLayout;
        filterRow->setSpacing(10);

        m_searchEdit = new QLineEdit(this);
        m_searchEdit->setPlaceholderText("🔍  Search user, action, resource…");
        m_searchEdit->setClearButtonEnabled(true);
        connect(m_searchEdit, &QLineEdit::textChanged, this, &AuditWidget::applyFilter);

        m_severityCb = new QComboBox(this);
        m_severityCb->addItems({"All Severities","INFO","WARN","ERROR","CRITICAL"});
        m_severityCb->setFixedWidth(160);
        connect(m_severityCb, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &AuditWidget::applyFilter);

        m_outcomeCb = new QComboBox(this);
        m_outcomeCb->addItems({"All Outcomes","SUCCESS","DENIED","FAILED"});
        m_outcomeCb->setFixedWidth(140);
        connect(m_outcomeCb, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &AuditWidget::applyFilter);

        m_countLbl = new QLabel("0 events", this);
        m_countLbl->setStyleSheet("font-size:12px; color:#8891b0;");

        filterRow->addWidget(m_searchEdit, 1);
        filterRow->addWidget(m_severityCb);
        filterRow->addWidget(m_outcomeCb);
        filterRow->addWidget(m_countLbl);
        root->addLayout(filterRow);

        // ── Table ────────────────────────────────────────────────────────────
        m_table = new QTableWidget(0, 7, this);
        m_table->setHorizontalHeaderLabels({
            "Timestamp", "Severity", "User", "Tenant", "Action", "Resource", "Outcome"
        });
        m_table->setAlternatingRowColors(true);
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->verticalHeader()->hide();
        m_table->setShowGrid(false);
        m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
        m_table->horizontalHeader()->setStretchLastSection(false);
        m_table->setSortingEnabled(true);
        root->addWidget(m_table, 1);
    }

    static QString severityColor(const QString& sev) {
        if (sev=="CRITICAL") return "#f87171";
        if (sev=="ERROR")    return "#fb923c";
        if (sev=="WARN")     return "#fbbf24";
        return "#8891b0";
    }

    static QString outcomeColor(const QString& out) {
        if (out=="SUCCESS") return "#4ade80";
        if (out=="DENIED")  return "#fbbf24";
        return "#f87171";
    }

    void appendEntry(const AuditEntry& e) {
        m_entries.prepend(e);  // newest first
        m_table->setSortingEnabled(false);
        m_table->insertRow(0);
        m_table->setRowHeight(0, 38);

        auto set = [&](int c, const QString& v, const QString& col="#e8eaf0") {
            auto* item = new QTableWidgetItem(v);
            item->setForeground(QColor(col));
            m_table->setItem(0, c, item);
        };
        set(0, e.timestamp, "#4a5068");
        auto* sevItem = new QTableWidgetItem("● " + e.severity);
        sevItem->setForeground(QColor(severityColor(e.severity)));
        QFont f; f.setBold(e.severity=="CRITICAL"||e.severity=="ERROR");
        sevItem->setFont(f);
        m_table->setItem(0, 1, sevItem);
        set(2, e.user);
        set(3, e.tenantId, "#8891b0");
        set(4, e.action);
        set(5, e.resource, "#8891b0");
        auto* outItem = new QTableWidgetItem(e.outcome);
        outItem->setForeground(QColor(outcomeColor(e.outcome)));
        m_table->setItem(0, 6, outItem);

        m_table->setSortingEnabled(true);
        updateCount();
    }

    void applyFilter() {
        const QString search = m_searchEdit->text().trimmed().toLower();
        const QString sevFilter = m_severityCb->currentIndex()==0 ? "" : m_severityCb->currentText();
        const QString outFilter = m_outcomeCb->currentIndex()==0 ? "" : m_outcomeCb->currentText();

        for (int r=0; r<m_table->rowCount(); ++r) {
            bool show = true;
            if (!search.isEmpty()) {
                bool found = false;
                for (int c=0; c<m_table->columnCount(); ++c) {
                    if (m_table->item(r,c) &&
                        m_table->item(r,c)->text().toLower().contains(search))
                    { found=true; break; }
                }
                show &= found;
            }
            if (!sevFilter.isEmpty() && m_table->item(r,1))
                show &= m_table->item(r,1)->text().contains(sevFilter);
            if (!outFilter.isEmpty() && m_table->item(r,6))
                show &= m_table->item(r,6)->text()==outFilter;
            m_table->setRowHidden(r, !show);
        }
        updateCount();
    }

    void updateCount() {
        int vis=0;
        for (int r=0; r<m_table->rowCount(); ++r)
            if (!m_table->isRowHidden(r)) ++vis;
        m_countLbl->setText(QString::number(vis) + " / " +
                            QString::number(m_table->rowCount()) + " events");
    }

    void clearLog() {
        m_table->setRowCount(0);
        m_entries.clear();
        updateCount();
    }

    void seedEvents() {
        struct Seed { QString sev; QString user; QString action; QString res; QString out; QString tid; };
        const QList<Seed> seeds = {
            {"INFO",    "alice",  "EXECUTE_COMMAND",    "grep_pipeline",    "SUCCESS",  "platform-eng"},
            {"INFO",    "bob",    "MANAGE_JOBS",        "job-j-0003",       "SUCCESS",  "data-science"},
            {"WARN",    "eve",    "READ_AUDIT_LOG",     "/audit/stream",    "DENIED",   "platform-eng"},
            {"ERROR",   "frank",  "EXECUTE_PIPELINE",   "sort_pipeline",    "FAILED",   "data-science"},
            {"INFO",    "carol",  "READ_CLUSTER_STATUS","/cluster/health",  "SUCCESS",  "security"},
            {"CRITICAL","unknown","EXECUTE_COMMAND",     "rm_pipeline",      "DENIED",   "qa-automation"},
            {"INFO",    "dave",   "MANAGE_TENANTS",     "qa-automation",    "SUCCESS",  "qa-automation"},
            {"WARN",    "alice",  "MANAGE_RESOURCES",   "worker-02",        "SUCCESS",  "platform-eng"},
        };
        for (int i=seeds.size()-1; i>=0; --i) {
            const auto& s = seeds[i];
            appendEntry({QDateTime::currentDateTime().addSecs(-(seeds.size()-i)*45).toString("yyyy-MM-dd hh:mm:ss"),
                         s.sev, s.user, s.action, s.res, s.tid, s.out});
        }
    }

    void appendRandomEvent() {
        static const QStringList users   {"alice","bob","carol","dave","eve","unknown"};
        static const QStringList actions {"EXECUTE_COMMAND","READ_SYSTEM_METRICS",
                                          "MANAGE_JOBS","READ_AUDIT_LOG","EXECUTE_PIPELINE"};
        static const QStringList sevs    {"INFO","INFO","INFO","WARN","ERROR","CRITICAL"};
        static const QStringList outs    {"SUCCESS","SUCCESS","SUCCESS","DENIED","FAILED"};
        static const QStringList tenants {"platform-eng","data-science","security","qa-automation"};
        static const QStringList res     {"grep_pipeline","job-queue","worker-01","/metrics","/exec"};

        auto pick = [](const QStringList& l) {
            return l[QRandomGenerator::global()->bounded(l.size())];
        };
        appendEntry({
            QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss"),
            pick(sevs), pick(users), pick(actions), pick(res), pick(tenants), pick(outs)
        });
        applyFilter();
    }

    QTableWidget* m_table      = nullptr;
    QLineEdit*    m_searchEdit = nullptr;
    QComboBox*    m_severityCb = nullptr;
    QComboBox*    m_outcomeCb  = nullptr;
    QLabel*       m_countLbl   = nullptr;
    QLabel*       m_liveLbl    = nullptr;
    QTimer*       m_ticker     = nullptr;
    QList<AuditEntry> m_entries;
};
