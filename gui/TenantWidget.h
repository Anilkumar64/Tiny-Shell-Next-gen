#pragma once
#include <QWidget>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTableWidget>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QUuid>
#include <QProgressBar>
#include "TshStyle.h"

// Multi-tenant management — mirrors MultiTenantManager.h data structures.
class TenantWidget : public QWidget {
    Q_OBJECT

    struct Tenant {
        QString id;
        QString name;
        QString owner;
        bool    active{true};
        int     quotaJobs{100};
        int     quotaMemMb{1024};
        double  quotaCpuPct{50.0};
    };

    struct TenantUser {
        QString userId;
        QString username;
        QString tenantId;
        QString role;
        bool    active{true};
    };

public:
    explicit TenantWidget(QWidget* parent = nullptr) : QWidget(parent) {
        buildUi();
        seedData();
    }

private:
    void buildUi() {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(28, 24, 28, 24);
        root->setSpacing(16);

        auto* titleLbl = new QLabel("Multi-Tenant Management", this);
        titleLbl->setStyleSheet("font-size:22px; font-weight:700; color:#e8eaf0;");
        root->addWidget(titleLbl);

        auto* tabs = new QTabWidget(this);
        root->addWidget(tabs, 1);

        tabs->addTab(buildTenantsTab(), "Tenants");
        tabs->addTab(buildUsersTab(),   "Users");
    }

    // ── Tenants Tab ──────────────────────────────────────────────────────────
    QWidget* buildTenantsTab() {
        auto* w = new QWidget;
        auto* vbox = new QVBoxLayout(w);
        vbox->setContentsMargins(16, 16, 16, 16);
        vbox->setSpacing(12);

        auto* toolbar = new QHBoxLayout;
        auto* addBtn = new QPushButton("+ New Tenant", w);
        addBtn->setProperty("role", "primary"); addBtn->setFixedHeight(34);
        connect(addBtn, &QPushButton::clicked, this, &TenantWidget::showNewTenantDialog);
        toolbar->addStretch();
        toolbar->addWidget(addBtn);
        vbox->addLayout(toolbar);

        m_tenantTable = new QTableWidget(0, 7, w);
        m_tenantTable->setHorizontalHeaderLabels({
            "ID", "Name", "Owner", "Status", "Max Jobs", "Memory MB", "CPU %"
        });
        setupTable(m_tenantTable);
        m_tenantTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        m_tenantTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        vbox->addWidget(m_tenantTable, 1);

        auto* bottomRow = new QHBoxLayout;
        m_tenantInfoLbl = new QLabel("Select a tenant.", w);
        m_tenantInfoLbl->setStyleSheet("font-size:12px; color:#8891b0;");
        auto* editBtn = new QPushButton("Edit Quotas…", w);
        editBtn->setFixedHeight(32);
        connect(editBtn, &QPushButton::clicked, this, &TenantWidget::editTenantQuotas);
        auto* toggleBtn = new QPushButton("Toggle Active", w);
        toggleBtn->setFixedHeight(32);
        connect(toggleBtn, &QPushButton::clicked, this, &TenantWidget::toggleTenantActive);
        auto* delBtn = new QPushButton("✕ Delete", w);
        delBtn->setProperty("role","danger"); delBtn->setFixedHeight(32);
        connect(delBtn, &QPushButton::clicked, this, &TenantWidget::deleteTenant);
        connect(m_tenantTable, &QTableWidget::itemSelectionChanged, this,
            [this]{ m_tenantInfoLbl->setText(
                m_tenantTable->currentRow()>=0 ?
                "Selected: " + m_tenantTable->item(m_tenantTable->currentRow(),1)->text() :
                "Select a tenant."); });

        bottomRow->addWidget(m_tenantInfoLbl);
        bottomRow->addStretch();
        bottomRow->addWidget(editBtn);
        bottomRow->addWidget(toggleBtn);
        bottomRow->addWidget(delBtn);
        vbox->addLayout(bottomRow);
        return w;
    }

    // ── Users Tab ────────────────────────────────────────────────────────────
    QWidget* buildUsersTab() {
        auto* w = new QWidget;
        auto* vbox = new QVBoxLayout(w);
        vbox->setContentsMargins(16, 16, 16, 16);
        vbox->setSpacing(12);

        auto* toolbar = new QHBoxLayout;
        m_tenantFilter = new QComboBox(w);
        m_tenantFilter->addItem("All Tenants");
        connect(m_tenantFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &TenantWidget::filterUsers);
        auto* addUserBtn = new QPushButton("+ Add User", w);
        addUserBtn->setProperty("role","primary"); addUserBtn->setFixedHeight(34);
        connect(addUserBtn, &QPushButton::clicked, this, &TenantWidget::showAddUserDialog);
        toolbar->addWidget(new QLabel("Filter:", w));
        toolbar->addWidget(m_tenantFilter, 1);
        toolbar->addWidget(addUserBtn);
        vbox->addLayout(toolbar);

        m_userTable = new QTableWidget(0, 5, w);
        m_userTable->setHorizontalHeaderLabels({
            "User ID", "Username", "Tenant", "Role", "Status"
        });
        setupTable(m_userTable);
        m_userTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        m_userTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        vbox->addWidget(m_userTable, 1);

        auto* bottomRow = new QHBoxLayout;
        auto* delUserBtn = new QPushButton("✕ Remove User", w);
        delUserBtn->setProperty("role","danger"); delUserBtn->setFixedHeight(32);
        connect(delUserBtn, &QPushButton::clicked, this, &TenantWidget::deleteUser);
        bottomRow->addStretch();
        bottomRow->addWidget(delUserBtn);
        vbox->addLayout(bottomRow);
        return w;
    }

    static void setupTable(QTableWidget* t) {
        t->setAlternatingRowColors(true);
        t->setSelectionBehavior(QAbstractItemView::SelectRows);
        t->setEditTriggers(QAbstractItemView::NoEditTriggers);
        t->verticalHeader()->hide();
        t->setShowGrid(false);
        t->horizontalHeader()->setStretchLastSection(true);
    }

    void seedData() {
        auto addT = [&](const QString& name, const QString& owner,
                        int jobs, int mem, double cpu)
        {
            Tenant t;
            t.id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
            t.name=name; t.owner=owner;
            t.quotaJobs=jobs; t.quotaMemMb=mem; t.quotaCpuPct=cpu;
            addTenantRow(t);
            m_tenantFilter->addItem(name);
        };
        addT("platform-eng",  "alice",  200, 4096, 80.0);
        addT("data-science",  "bob",    100, 8192, 60.0);
        addT("security",      "carol",   50, 1024, 30.0);
        addT("qa-automation", "dave",   150, 2048, 50.0);

        auto addU = [&](const QString& uname, const QString& tid, const QString& role){
            TenantUser u;
            u.userId=QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
            u.username=uname; u.tenantId=tid; u.role=role;
            m_users.append(u);
            addUserRow(u);
        };
        addU("alice",  m_tenants[0].id, "admin");
        addU("bob",    m_tenants[1].id, "admin");
        addU("carol",  m_tenants[2].id, "operator");
        addU("dave",   m_tenants[3].id, "admin");
        addU("eve",    m_tenants[0].id, "viewer");
        addU("frank",  m_tenants[1].id, "scheduler");
    }

    void addTenantRow(const Tenant& t) {
        m_tenants.append(t);
        const int row = m_tenantTable->rowCount();
        m_tenantTable->insertRow(row);
        m_tenantTable->setRowHeight(row, 40);
        auto set = [&](int c, const QString& v, const QString& col="#e8eaf0"){
            auto* i = new QTableWidgetItem(v);
            i->setForeground(QColor(col)); m_tenantTable->setItem(row,c,i);};
        set(0, t.id,   "#4a5068");
        set(1, t.name);
        set(2, t.owner,"#8891b0");
        auto* si = new QTableWidgetItem(t.active ? "● Active" : "○ Inactive");
        si->setForeground(QColor(t.active ? "#4ade80" : "#f87171"));
        m_tenantTable->setItem(row, 3, si);
        set(4, QString::number(t.quotaJobs));
        set(5, QString::number(t.quotaMemMb) + " MB");
        set(6, QString::number(t.quotaCpuPct,'f',1) + "%");
    }

    void addUserRow(const TenantUser& u) {
        // find tenant name
        QString tenantName = u.tenantId;
        for (const auto& t : m_tenants)
            if (t.id == u.tenantId) { tenantName = t.name; break; }

        const int row = m_userTable->rowCount();
        m_userTable->insertRow(row);
        m_userTable->setRowHeight(row, 40);
        auto set = [&](int c, const QString& v, const QString& col="#e8eaf0"){
            auto* i = new QTableWidgetItem(v);
            i->setForeground(QColor(col)); m_userTable->setItem(row,c,i);};
        set(0, u.userId, "#4a5068");
        set(1, u.username);
        set(2, tenantName, "#8891b0");
        set(3, u.role,     "#00b4d8");
        auto* si = new QTableWidgetItem(u.active ? "● Active" : "○ Inactive");
        si->setForeground(QColor(u.active ? "#4ade80" : "#8891b0"));
        m_userTable->setItem(row, 4, si);
    }

    void showNewTenantDialog() {
        auto* dlg = new QDialog(this);
        dlg->setWindowTitle("New Tenant"); dlg->setFixedWidth(380);
        dlg->setStyleSheet("background:#1c1f2b; color:#e8eaf0;");
        auto* form = new QFormLayout(dlg);
        form->setContentsMargins(20,20,20,16); form->setSpacing(12);
        auto* nameE  = new QLineEdit(dlg); nameE->setPlaceholderText("team-name");
        auto* ownerE = new QLineEdit(dlg); ownerE->setPlaceholderText("username");
        auto* jobsSp = new QSpinBox(dlg);    jobsSp->setRange(1,10000); jobsSp->setValue(100);
        auto* memSp  = new QSpinBox(dlg);    memSp->setRange(128,65536); memSp->setValue(1024);
        auto* cpuSp  = new QDoubleSpinBox(dlg); cpuSp->setRange(1,100); cpuSp->setValue(50);
        form->addRow("Tenant name:", nameE);
        form->addRow("Owner:",       ownerE);
        form->addRow("Max jobs:",    jobsSp);
        form->addRow("Memory MB:",   memSp);
        form->addRow("CPU %:",       cpuSp);
        auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,dlg);
        btns->button(QDialogButtonBox::Ok)->setProperty("role","primary");
        connect(btns,&QDialogButtonBox::accepted,dlg,&QDialog::accept);
        connect(btns,&QDialogButtonBox::rejected,dlg,&QDialog::reject);
        form->addRow(btns);
        if (dlg->exec()==QDialog::Accepted) {
            Tenant t;
            t.id=QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
            t.name=nameE->text().trimmed(); t.owner=ownerE->text().trimmed();
            t.quotaJobs=jobsSp->value(); t.quotaMemMb=memSp->value(); t.quotaCpuPct=cpuSp->value();
            addTenantRow(t);
            m_tenantFilter->addItem(t.name);
        }
        dlg->deleteLater();
    }

    void showAddUserDialog() {
        auto* dlg = new QDialog(this);
        dlg->setWindowTitle("Add User"); dlg->setFixedWidth(360);
        dlg->setStyleSheet("background:#1c1f2b; color:#e8eaf0;");
        auto* form = new QFormLayout(dlg);
        form->setContentsMargins(20,20,20,16); form->setSpacing(12);
        auto* nameE = new QLineEdit(dlg); nameE->setPlaceholderText("username");
        auto* tenantCb = new QComboBox(dlg);
        for (const auto& t : m_tenants) tenantCb->addItem(t.name);
        auto* roleCb = new QComboBox(dlg);
        roleCb->addItems({"admin","operator","viewer","auditor","scheduler"});
        form->addRow("Username:", nameE);
        form->addRow("Tenant:",   tenantCb);
        form->addRow("Role:",     roleCb);
        auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,dlg);
        btns->button(QDialogButtonBox::Ok)->setProperty("role","primary");
        connect(btns,&QDialogButtonBox::accepted,dlg,&QDialog::accept);
        connect(btns,&QDialogButtonBox::rejected,dlg,&QDialog::reject);
        form->addRow(btns);
        if (dlg->exec()==QDialog::Accepted && !nameE->text().trimmed().isEmpty()) {
            TenantUser u;
            u.userId=QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
            u.username=nameE->text().trimmed();
            u.tenantId=m_tenants.value(tenantCb->currentIndex()).id;
            u.role=roleCb->currentText();
            m_users.append(u); addUserRow(u);
        }
        dlg->deleteLater();
    }

    void editTenantQuotas() {
        const int row = m_tenantTable->currentRow();
        if (row < 0) return;
        auto& t = m_tenants[row];
        auto* dlg = new QDialog(this);
        dlg->setWindowTitle("Edit Quotas: " + t.name); dlg->setFixedWidth(340);
        dlg->setStyleSheet("background:#1c1f2b; color:#e8eaf0;");
        auto* form = new QFormLayout(dlg);
        form->setContentsMargins(20,20,20,16); form->setSpacing(12);
        auto* jobsSp = new QSpinBox(dlg);    jobsSp->setRange(1,10000); jobsSp->setValue(t.quotaJobs);
        auto* memSp  = new QSpinBox(dlg);    memSp->setRange(128,65536); memSp->setValue(t.quotaMemMb);
        auto* cpuSp  = new QDoubleSpinBox(dlg); cpuSp->setRange(1,100); cpuSp->setValue(t.quotaCpuPct);
        form->addRow("Max jobs:",  jobsSp);
        form->addRow("Memory MB:", memSp);
        form->addRow("CPU %:",     cpuSp);
        auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,dlg);
        btns->button(QDialogButtonBox::Ok)->setProperty("role","primary");
        connect(btns,&QDialogButtonBox::accepted,dlg,&QDialog::accept);
        connect(btns,&QDialogButtonBox::rejected,dlg,&QDialog::reject);
        form->addRow(btns);
        if (dlg->exec()==QDialog::Accepted) {
            t.quotaJobs=jobsSp->value(); t.quotaMemMb=memSp->value(); t.quotaCpuPct=cpuSp->value();
            m_tenantTable->item(row,4)->setText(QString::number(t.quotaJobs));
            m_tenantTable->item(row,5)->setText(QString::number(t.quotaMemMb)+" MB");
            m_tenantTable->item(row,6)->setText(QString::number(t.quotaCpuPct,'f',1)+"%");
        }
        dlg->deleteLater();
    }

    void toggleTenantActive() {
        const int row = m_tenantTable->currentRow();
        if (row < 0) return;
        m_tenants[row].active = !m_tenants[row].active;
        const bool a = m_tenants[row].active;
        auto* si = m_tenantTable->item(row,3);
        si->setText(a ? "● Active" : "○ Inactive");
        si->setForeground(QColor(a ? "#4ade80" : "#f87171"));
    }

    void deleteTenant() {
        const int row = m_tenantTable->currentRow();
        if (row < 0) return;
        if (QMessageBox::question(this,"Delete Tenant",
            "Delete tenant <b>"+m_tenants[row].name+"</b>?",
            QMessageBox::Yes|QMessageBox::Cancel)==QMessageBox::Yes) {
            m_tenantTable->removeRow(row);
            m_tenants.removeAt(row);
        }
    }

    void filterUsers() {
        const int idx = m_tenantFilter->currentIndex();
        for (int r=0; r<m_userTable->rowCount(); ++r) {
            const QString uTenant = m_userTable->item(r,2)->text();
            const bool show = (idx==0) || (uTenant == m_tenantFilter->currentText());
            m_userTable->setRowHidden(r, !show);
        }
    }

    void deleteUser() {
        const int row = m_userTable->currentRow();
        if (row<0) return;
        m_userTable->removeRow(row);
        if (row < m_users.size()) m_users.removeAt(row);
    }

    QTableWidget* m_tenantTable = nullptr;
    QTableWidget* m_userTable   = nullptr;
    QComboBox*    m_tenantFilter= nullptr;
    QLabel*       m_tenantInfoLbl = nullptr;
    QList<Tenant>     m_tenants;
    QList<TenantUser> m_users;
};
