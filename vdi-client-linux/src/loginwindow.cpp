#include "loginwindow.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QIcon>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QDebug>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QOverload>
#include <QScreen>
#include <QRegularExpression>

// 构造函数：初始化登录窗口
LoginWindow::LoginWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_serverEdit(nullptr)
    , m_usernameEdit(nullptr)
    , m_passwordEdit(nullptr)
    , m_rememberPasswordCheckBox(nullptr)
    , m_autoLoginCheckBox(nullptr)
    , m_languageButton(nullptr)
    , m_languageMenu(nullptr)
    , m_loginButton(nullptr)
    , m_statusLabel(nullptr)
    , m_networkManager(nullptr)
    , m_titleLabel(nullptr)
    , m_vmListWidget(nullptr)
    , m_vmListContainer(nullptr)
    , m_backButton(nullptr)
    , m_refreshButton(nullptr)
    , m_vmTitleLabel(nullptr)
    , m_vmStatusLabel(nullptr)
    , m_stackedWidget(nullptr)
    , m_currentLanguage("en_US")
    , m_rdpProcess(nullptr)
    , m_heartbeatTimer(nullptr)
{
    qInfo() << "LoginWindow constructor called";

    // 初始化多语言翻译字典
    initTranslations();

    // 配置SSL，禁用证书验证（用于开发环境）
    QSslConfiguration sslConfig = QSslConfiguration::defaultConfiguration();
    sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
    QSslConfiguration::setDefaultConfiguration(sslConfig);

    // 创建网络访问管理器，用于处理HTTP请求
    m_networkManager = new QNetworkAccessManager(this);

    // 设置用户界面
    setupUi();

    // 加载保存的设置
    loadSettings();

    // 检查是否需要自动登录
    if (m_autoLoginCheckBox->isChecked() &&
        !m_serverEdit->text().trimmed().isEmpty() &&
        !m_usernameEdit->text().trimmed().isEmpty() &&
        !m_passwordEdit->text().isEmpty()) {
        // 延迟一下再自动登录，确保UI完全加载
        QTimer::singleShot(500, this, [this]() {
            onLoginClicked();
        });
    }
}

// 析构函数：清理资源
LoginWindow::~LoginWindow()
{
    if (m_rdpProcess) {
        if (m_rdpProcess->state() == QProcess::Running) {
            m_rdpProcess->terminate();
            if (!m_rdpProcess->waitForFinished(3000)) {
                m_rdpProcess->kill();
            }
        }
        delete m_rdpProcess;
        m_rdpProcess = nullptr;
    }
}

// 设置主窗口用户界面
// 使用QStackedWidget实现登录页面和虚拟机列表页面的切换
void LoginWindow::setupUi()
{
    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    centralWidget->setStyleSheet(
        "QWidget { background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #dbeafe, stop:1 #93c5fd); }"
    );

    QVBoxLayout *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setSpacing(0);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    m_stackedWidget = new QStackedWidget(this);
    mainLayout->addWidget(m_stackedWidget);

    QWidget *loginWidget = new QWidget();
    setupLoginUi(loginWidget);
    m_stackedWidget->addWidget(loginWidget);

    QWidget *vmListWidget = new QWidget();
    setupVmListUi(vmListWidget);
    m_stackedWidget->addWidget(vmListWidget);

    m_stackedWidget->setCurrentIndex(0);

    setWindowTitle("VDI Client");
    setWindowIcon(QIcon(":/logo.png"));
    resize(600, 650);
    setMinimumSize(480, 560);
}

// 设置登录页面用户界面
void LoginWindow::setupLoginUi(QWidget *widget)
{
    QVBoxLayout *mainLayout = new QVBoxLayout(widget);
    mainLayout->setSpacing(0);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    QWidget *containerWidget = new QWidget();
    containerWidget->setStyleSheet(
        "QWidget {"
        "   background-color: #f5f7fa;"
        "   border-radius: 12px;"
        "}"
    );
    containerWidget->setMaximumWidth(420);

    QVBoxLayout *containerLayout = new QVBoxLayout(containerWidget);
    containerLayout->setSpacing(25);
    containerLayout->setContentsMargins(40, 40, 40, 40);

    m_titleLabel = new QLabel(translate("title"));
    m_titleLabel->setAlignment(Qt::AlignCenter);
    m_titleLabel->setStyleSheet(
        "font-size: 24px;"
        "font-weight: bold;"
        "color: #2c3e50;"
        "padding: 10px 0;"
    );
    containerLayout->addWidget(m_titleLabel);

    QString inputStyle = (
        "QLineEdit {"
        "   background-color: white;"
        "   border: 2px solid #e1e8ed;"
        "   border-radius: 8px;"
        "   padding: 12px 15px;"
        "   font-size: 14px;"
        "   color: #4a5568;"
        "}"
        "QLineEdit:focus {"
        "   border: 2px solid #3b82f6;"
        "   background-color: white;"
        "}"
    );

    m_serverEdit = new QLineEdit();
    m_serverEdit->setPlaceholderText(translate("server"));
    m_serverEdit->setMinimumHeight(45);
    m_serverEdit->setStyleSheet(inputStyle);
    containerLayout->addWidget(m_serverEdit);

    m_usernameEdit = new QLineEdit();
    m_usernameEdit->setPlaceholderText(translate("username"));
    m_usernameEdit->setMinimumHeight(45);
    m_usernameEdit->setStyleSheet(inputStyle);
    containerLayout->addWidget(m_usernameEdit);

    m_passwordEdit = new QLineEdit();
    m_passwordEdit->setPlaceholderText(translate("password"));
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setMinimumHeight(45);
    m_passwordEdit->setStyleSheet(inputStyle);
    containerLayout->addWidget(m_passwordEdit);

    QHBoxLayout *optionsLayout = new QHBoxLayout();
    optionsLayout->setSpacing(15);

    QString checkboxStyle = (
        "QCheckBox {"
        "   font-size: 13px;"
        "   color: #64748b;"
        "   spacing: 5px;"
        "}"
        "QCheckBox::indicator {"
        "   width: 18px;"
        "   height: 18px;"
        "   border: 2px solid #cbd5e1;"
        "   border-radius: 4px;"
        "   background-color: white;"
        "}"
        "QCheckBox::indicator:checked {"
        "   background-color: #3b82f6;"
        "   border-color: #3b82f6;"
        "}"
        "QCheckBox::indicator:checked:hover {"
        "   background-color: #2563eb;"
        "}"
    );

    m_rememberPasswordCheckBox = new QCheckBox(translate("remember_password"));
    m_rememberPasswordCheckBox->setStyleSheet(checkboxStyle);
    optionsLayout->addWidget(m_rememberPasswordCheckBox);

    m_autoLoginCheckBox = new QCheckBox(translate("auto_login"));
    m_autoLoginCheckBox->setStyleSheet(checkboxStyle);
    connect(m_autoLoginCheckBox, &QCheckBox::checkStateChanged, this, &LoginWindow::onAutoLoginChanged);
    optionsLayout->addWidget(m_autoLoginCheckBox);

    optionsLayout->addStretch();

    m_languageButton = new QPushButton("\xF0\x9F\x8C\x90");
    m_languageButton->setMinimumSize(45, 38);
    m_languageButton->setMaximumSize(45, 38);
    m_languageButton->setStyleSheet(
        "QPushButton {"
        "   background: transparent;"
        "   border: none;"
        "   font-size: 20px;"
        "   padding: 6px 0px;"
        "}"
        "QPushButton::menu-indicator {"
        "   image: none;"
        "}"
        "QPushButton:hover {"
        "   background: transparent;"
        "}"
        "QPushButton:pressed {"
        "   background: transparent;"
        "}"
    );
    connect(m_languageButton, &QPushButton::clicked, this, &LoginWindow::onLanguageButtonClicked);
    optionsLayout->addWidget(m_languageButton);

    m_languageMenu = new QMenu(this);
    m_languageMenu->setStyleSheet(
        "QMenu {"
        "   background-color: white;"
        "   border: 1px solid #e1e8ed;"
        "   border-radius: 8px;"
        "   padding: 5px;"
        "}"
        "QMenu::item {"
        "   padding: 8px 20px;"
        "   border-radius: 4px;"
        "}"
        "QMenu::item:selected {"
        "   background-color: #3b82f6;"
        "   color: white;"
        "}"
    );

    QAction *englishAction = m_languageMenu->addAction("English");
    englishAction->setData("en_US");

    QAction *chineseAction = m_languageMenu->addAction("\xE4\xB8\xAD\xE6\x96\x87");
    chineseAction->setData("zh_CN");

    QAction *traditionalChineseAction = m_languageMenu->addAction("\xE7\xB9\x81\xE9\xAB\x94\xE4\xB8\xAD\xE6\x96\x87");
    traditionalChineseAction->setData("zh_TW");

    QAction *japaneseAction = m_languageMenu->addAction("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
    japaneseAction->setData("ja_JP");

    connect(m_languageMenu, &QMenu::triggered, this, &LoginWindow::onLanguageSelected);

    m_languageButton->setMenu(m_languageMenu);

    containerLayout->addLayout(optionsLayout);

    m_loginButton = new QPushButton(translate("login"));
    m_loginButton->setMinimumHeight(50);
    m_loginButton->setStyleSheet(
        "QPushButton {"
        "   background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #3b82f6, stop:1 #2563eb);"
        "   color: white;"
        "   border: none;"
        "   border-radius: 8px;"
        "   font-size: 16px;"
        "   font-weight: bold;"
        "   padding: 12px;"
        "}"
        "QPushButton:hover {"
        "   background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #2563eb, stop:1 #1d4ed8);"
        "}"
        "QPushButton:pressed {"
        "   background-color: #1e40af;"
        "}"
        "QPushButton:disabled {"
        "   background-color: #cbd5e1;"
        "   color: #94a3b8;"
        "}"
    );
    connect(m_loginButton, &QPushButton::clicked, this, &LoginWindow::onLoginClicked);
    containerLayout->addWidget(m_loginButton);

    m_statusLabel = new QLabel();
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setStyleSheet(
        "font-size: 13px;"
        "color: #dc3545;"
        "padding: 5px;"
    );
    m_statusLabel->hide();
    containerLayout->addWidget(m_statusLabel);

    containerLayout->addStretch();

    mainLayout->addStretch();
    mainLayout->addWidget(containerWidget, 0, Qt::AlignCenter);
    mainLayout->addStretch();
}

// 设置虚拟机列表页面用户界面
void LoginWindow::setupVmListUi(QWidget *widget)
{
    QVBoxLayout *mainLayout = new QVBoxLayout(widget);
    mainLayout->setSpacing(0);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    widget->setStyleSheet(
        "QWidget { background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #dbeafe, stop:1 #93c5fd); }"
    );

    QWidget *containerWidget = new QWidget();
    containerWidget->setStyleSheet(
        "QWidget {"
        "   background-color: #f5f7fa;"
        "   border-radius: 12px;"
        "}"
    );
    containerWidget->setMaximumWidth(800);

    QVBoxLayout *containerLayout = new QVBoxLayout(containerWidget);
    containerLayout->setSpacing(25);
    containerLayout->setContentsMargins(40, 40, 40, 40);

    m_vmTitleLabel = new QLabel(translate("vm_list_title"));
    m_vmTitleLabel->setAlignment(Qt::AlignCenter);
    m_vmTitleLabel->setStyleSheet(
        "font-size: 24px;"
        "font-weight: bold;"
        "color: #2c3e50;"
        "padding: 10px 0;"
    );
    containerLayout->addWidget(m_vmTitleLabel);

    m_vmListWidget = new QWidget();
    m_vmListWidget->setStyleSheet(
        "QWidget {"
        "   background-color: #f5f7fa;"
        "   border-radius: 8px;"
        "}"
    );

    QVBoxLayout *vmListLayout = new QVBoxLayout(m_vmListWidget);
    vmListLayout->setSpacing(15);
    vmListLayout->setContentsMargins(20, 20, 20, 20);

    containerLayout->addWidget(m_vmListWidget);

    // 创建按钮容器，与虚拟机列表项保持相同的边距
    QWidget *buttonContainerWidget = new QWidget();
    buttonContainerWidget->setStyleSheet(
        "QWidget {"
        "   background-color: #f5f7fa;"
        "   border-radius: 8px;"
        "}"
    );

    QVBoxLayout *buttonContainerLayout = new QVBoxLayout(buttonContainerWidget);
    buttonContainerLayout->setSpacing(15);
    buttonContainerLayout->setContentsMargins(20, 20, 20, 20);

    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->setSpacing(15);

    QString buttonStyle = (
        "QPushButton {"
        "   background-color: white;"
        "   border: 2px solid #e1e8ed;"
        "   border-radius: 8px;"
        "   font-size: 14px;"
        "   padding: 10px 20px;"
        "   color: #4400ff;"
        "}"
        "QPushButton:hover {"
        "   border-color: #3b82f6;"
        "   background-color: #f0f9ff;"
        "}"
        "QPushButton:pressed {"
        "   background-color: #e1e8ed;"
        "}"
        "QPushButton:disabled {"
        "   background-color: #cbd5e1;"
        "   color: #94a3b8;"
        "}"
    );

    m_backButton = new QPushButton(translate("back"));
    m_backButton->setStyleSheet(buttonStyle);
    connect(m_backButton, &QPushButton::clicked, this, &LoginWindow::onBackClicked);
    buttonLayout->addWidget(m_backButton);

    m_changePasswordButton = new QPushButton(translate("change_password"));
    m_changePasswordButton->setStyleSheet(buttonStyle);
    connect(m_changePasswordButton, &QPushButton::clicked, this, &LoginWindow::onChangePasswordClicked);
    buttonLayout->addWidget(m_changePasswordButton);

    buttonLayout->addStretch();

    m_refreshButton = new QPushButton(translate("refresh"));
    m_refreshButton->setStyleSheet(buttonStyle);
    connect(m_refreshButton, &QPushButton::clicked, this, &LoginWindow::onRefreshClicked);
    buttonLayout->addWidget(m_refreshButton);

    buttonContainerLayout->addLayout(buttonLayout);
    containerLayout->addWidget(buttonContainerWidget);

    m_vmStatusLabel = new QLabel();
    m_vmStatusLabel->setAlignment(Qt::AlignCenter);
    m_vmStatusLabel->setStyleSheet(
        "font-size: 13px;"
        "color: #64748b;"
        "padding: 5px;"
    );
    m_vmStatusLabel->hide();
    containerLayout->addWidget(m_vmStatusLabel);

    mainLayout->addStretch();
    mainLayout->addWidget(containerWidget, 0, Qt::AlignCenter);
    mainLayout->addStretch();
}

// 创建虚拟机列表项控件
QFrame* LoginWindow::createVmItemWidget(const QString &vmName, const QString &vmId, const QString &status)
{
    QFrame *frame = new QFrame();
    frame->setStyleSheet(
        "QFrame {"
        "   background-color: #f5f7fa;"
        "   border: 2px solid #e1e8ed;"
        "   border-radius: 8px;"
        "   padding: 20px;"
        "}"
    );

    QVBoxLayout *frameLayout = new QVBoxLayout(frame);
    frameLayout->setSpacing(15);
    frameLayout->setContentsMargins(0, 0, 0, 0);

    QHBoxLayout *headerLayout = new QHBoxLayout();
    headerLayout->setSpacing(15);

    QLabel *nameLabel = new QLabel(vmName);
    nameLabel->setStyleSheet(
        "font-size: 20px;"
        "font-weight: bold;"
        "color: #1e40af;"
        "background-color: #dbeafe;"
        "padding: 8px 12px;"
        "border-radius: 6px;"
    );
    headerLayout->addWidget(nameLabel);

    headerLayout->addStretch();

    QString statusText = status.isEmpty() ? translate("status_unknown") : status;
    QString statusColor = "#64748b";
    if (status == "running") {
        statusText = translate("status_running");
        statusColor = "#10b981";
    } else if (status == "stopped") {
        statusText = translate("status_stopped");
        statusColor = "#ef4444";
    } else if (status == "paused") {
        statusText = translate("status_paused");
        statusColor = "#f59e0b";
    }

    QLabel *statusLabel = new QLabel(statusText);
    statusLabel->setStyleSheet(
        "font-size: 14px;"
        "font-weight: bold;"
        "color: " + statusColor + ";"
        "padding: 8px 15px;"
        "background-color: white;"
        "border-radius: 6px;"
        "border: 2px solid " + statusColor + ";"
    );
    headerLayout->addWidget(statusLabel);

    m_vmStatusLabels[vmId] = statusLabel;

    frameLayout->addLayout(headerLayout);

    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->setSpacing(15);

    QString actionButtonStyle = (
        "QPushButton {"
        "   background-color: #93c5fd;"
        "   border: none;"
        "   border-radius: 6px;"
        "   font-size: 15px;"
        "   padding: 12px 24px;"
        "   color: white;"
        "}"
        "QPushButton:hover {"
        "   background-color: #60a5fa;"
        "}"
        "QPushButton:pressed {"
        "   background-color: #1e40af;"
        "}"
        "QPushButton:disabled {"
        "   background-color: #cbd5e1;"
        "   color: #94a3b8;"
        "}"
    );

    QPushButton *startButton = new QPushButton(translate("start"));
    startButton->setStyleSheet(actionButtonStyle);
    connect(startButton, &QPushButton::clicked, this, [this, vmId]() {
        onVmStartClicked(vmId);
    });
    buttonLayout->addWidget(startButton);

    m_vmStartButtons[vmId] = startButton;

    QPushButton *stopButton = new QPushButton(translate("stop"));
    stopButton->setStyleSheet(actionButtonStyle);
    connect(stopButton, &QPushButton::clicked, this, [this, vmId]() {
        onVmStopClicked(vmId);
    });
    buttonLayout->addWidget(stopButton);

    QPushButton *restartButton = new QPushButton(translate("restart"));
    restartButton->setStyleSheet(actionButtonStyle);
    connect(restartButton, &QPushButton::clicked, this, [this, vmId]() {
        onVmRestartClicked(vmId);
    });
    buttonLayout->addWidget(restartButton);

    QPushButton *restoreButton = new QPushButton(translate("restore"));
    restoreButton->setStyleSheet(actionButtonStyle);
    restoreButton->setVisible(false);
    m_vmRestoreButtons[vmId] = restoreButton;
    connect(restoreButton, &QPushButton::clicked, this, [this, vmId]() {
        onVmRestoreClicked(vmId);
    });
    buttonLayout->addWidget(restoreButton);

    QPushButton *connectButton = new QPushButton(translate("connect_vm"));
    connectButton->setStyleSheet(actionButtonStyle);
    connect(connectButton, &QPushButton::clicked, this, [this, vmId]() {
        onVmConnectClicked(vmId);
    });
    buttonLayout->addWidget(connectButton);

    m_vmConnectButtons[vmId] = connectButton;

    frameLayout->addLayout(buttonLayout);

    return frame;
}

// 清空虚拟机列表控件中的所有子项
void LoginWindow::updateVmListWidget()
{
    QVBoxLayout *vmListLayout = qobject_cast<QVBoxLayout*>(m_vmListWidget->layout());
    if (vmListLayout) {
        while (vmListLayout->count()) {
            QLayoutItem *item = vmListLayout->takeAt(0);
            if (item->widget()) {
                item->widget()->deleteLater();
            }
            delete item;
        }
    }
}

// 处理登录按钮点击事件
void LoginWindow::onLoginClicked()
{
    QString server = m_serverEdit->text().trimmed();
    QString username = m_usernameEdit->text().trimmed();
    QString password = m_passwordEdit->text().trimmed();

    if (server.isEmpty()) {
        QMessageBox::warning(this, translate("alert_title"), translate("enter_server_address"));
        m_serverEdit->setFocus();
        return;
    }

    if (username.isEmpty()) {
        QMessageBox::warning(this, translate("alert_title"), translate("enter_username"));
        m_usernameEdit->setFocus();
        return;
    }

    if (password.isEmpty()) {
        QMessageBox::warning(this, translate("alert_title"), translate("enter_password"));
        m_passwordEdit->setFocus();
        return;
    }

    m_loginButton->setEnabled(false);
    m_loginButton->setText(translate("logging_in"));
    m_statusLabel->clear();
    m_statusLabel->show();
    checkServerHealth(server, username, password);
}

// 检查服务器健康状态
void LoginWindow::checkServerHealth(const QString &server, const QString &username, const QString &password)
{
    m_server = server;

    QString urlStr = buildApiUrl("api/v1/auth/health");

    QUrl url(urlStr);
    QNetworkRequest request(url);
    request.setTransferTimeout(5000);

    QSslConfiguration sslConfig = request.sslConfiguration();
    sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
    request.setSslConfiguration(sslConfig);

    m_healthCheckData.clear();

    QNetworkReply *reply = m_networkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, server, username, password]() {
        onHealthCheckReply(reply, server, username, password);
    });
    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        QByteArray data = reply->readAll();
        m_healthCheckData.append(data);
    });
}

// 处理服务器健康检查响应
void LoginWindow::onHealthCheckReply(QNetworkReply *reply, const QString &server, const QString &username, const QString &password)
{
    if (reply->error() != QNetworkReply::NoError) {
        reply->deleteLater();
        m_loginButton->setEnabled(true);
        m_loginButton->setText(translate("login"));
        m_statusLabel->setText(translate("network_error"));
        return;
    }

    QByteArray responseData = m_healthCheckData;
    reply->deleteLater();

    QString response = QString::fromUtf8(responseData).trimmed();

    if (response == "ok") {
        sendLoginRequest(server, username, password);
    } else {
        m_loginButton->setEnabled(true);
        m_loginButton->setText(translate("login"));
        m_statusLabel->setText(translate("login_failed"));
        QMessageBox::warning(this, translate("login_failed"), translate("server_unreachable"));
    }
}

bool LoginWindow::validateAndNormalizeServer(QString &server)
{
    QString input = server.trimmed();

    if (input.contains("://") || input.startsWith("http://") || input.startsWith("https://")) {
        return false;
    }

    if (input.endsWith("/")) {
        return false;
    }

    QRegularExpression ipWithPortRegex(R"(^((25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.){3}(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?):([0-9]{1,5})$)");
    QRegularExpression ipOnlyRegex(R"(^((25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.){3}(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$)");
    QRegularExpression domainWithPortRegex(R"(^([a-zA-Z0-9]([a-zA-Z0-9\-]{0,61}[a-zA-Z0-9])?\.)+[a-zA-Z]{2,}:([0-9]{1,5})$)");
    QRegularExpression domainOnlyRegex(R"(^([a-zA-Z0-9]([a-zA-Z0-9\-]{0,61}[a-zA-Z0-9])?\.)+[a-zA-Z]{2,}$)");

    if (ipWithPortRegex.match(input).hasMatch()) {
        server = input;
        return true;
    }

    if (ipOnlyRegex.match(input).hasMatch()) {
        server = input + ":443";
        return true;
    }

    if (domainWithPortRegex.match(input).hasMatch()) {
        server = input;
        return true;
    }

    if (domainOnlyRegex.match(input).hasMatch()) {
        server = input + ":443";
        return true;
    }

    return false;
}

void LoginWindow::sendLoginRequest(const QString &server, const QString &username, const QString &password)
{
    QString normalizedServer = server;
    if (!validateAndNormalizeServer(normalizedServer)) {
        m_loginButton->setEnabled(true);
        m_loginButton->setText(translate("login"));
        m_statusLabel->setText(translate("invalid_server_format"));
        m_statusLabel->show();
        QMessageBox::warning(this, translate("login_failed"), translate("invalid_server_format"));
        return;
    }

    QString urlStr = buildApiUrl("api/v1/auth/login");

    QUrl url(urlStr);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setTransferTimeout(5000);

    QSslConfiguration sslConfig = request.sslConfiguration();
    sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
    request.setSslConfiguration(sslConfig);

    QJsonObject json;
    json["username"] = username;
    json["password"] = password;
    json["client_type"] = "linux_client";
    json["login_server"] = normalizedServer;

    QJsonDocument doc(json);
    QByteArray data = doc.toJson();

    QNetworkReply *reply = m_networkManager->post(request, data);
    connect(reply, &QNetworkReply::errorOccurred, this, &LoginWindow::onLoginError);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onLoginReply(reply);
    });
}

// 处理登录响应
void LoginWindow::onLoginReply(QNetworkReply *reply)
{
    reply->deleteLater();

    QByteArray responseData = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(responseData);

    if (!doc.isNull() && doc.isObject()) {
        QJsonObject json = doc.object();

        if (json.contains("code")) {
            if (json["code"].toInt() == 0) {
                m_statusLabel->clear();
                m_statusLabel->hide();

                QJsonObject data = json["data"].toObject();
                m_token = data.value("token").toString();
                m_username = data.value("username").toString();
                m_server = m_serverEdit->text().trimmed();

                // 保存登录信息
                saveSettings();

                m_stackedWidget->setCurrentIndex(1);
                startHeartbeat();
                fetchVmList();
                return;
            } else if (json["code"].toInt() == 423) {
                m_loginButton->setEnabled(true);
                m_loginButton->setText(translate("login"));
                m_statusLabel->setText(translate("account_locked"));
                m_statusLabel->show();
                return;
            }
        }
    }

    // 处理其他错误情况
    m_loginButton->setEnabled(true);
    m_loginButton->setText(translate("login"));
    m_statusLabel->setText(translate("invalid_username_password"));
    m_statusLabel->show();
    QMessageBox::warning(this, translate("login_failed"), translate("invalid_username_password"));
}

// 处理登录错误
void LoginWindow::onLoginError(QNetworkReply::NetworkError error)
{
    Q_UNUSED(error);
}

// 处理自动登录复选框状态改变事件
void LoginWindow::onAutoLoginChanged(Qt::CheckState state)
{
    Q_UNUSED(state);
    // 立即保存设置，确保取消自动登录后不会在下次启动时自动登录
    saveSettings();
}

// 处理语言按钮点击事件
void LoginWindow::onLanguageButtonClicked()
{
    m_languageButton->showMenu();
}

// 处理语言选择事件
void LoginWindow::onLanguageSelected(QAction *action)
{
    QString languageCode = action->data().toString();
    updateLanguage(languageCode);
}

// 处理返回按钮点击事件
void LoginWindow::onBackClicked()
{
    stopHeartbeat();

    m_stackedWidget->setCurrentIndex(0);
    m_token.clear();
    m_username.clear();
    m_server.clear();

    // 恢复登录按钮状态
    m_loginButton->setEnabled(true);
    m_loginButton->setText(translate("login"));

    // 清空并隐藏状态标签
    m_statusLabel->clear();
    m_statusLabel->hide();

    // 只有在"记住密码"未被勾选时才清空密码
    if (!m_rememberPasswordCheckBox->isChecked()) {
        m_passwordEdit->clear();
    }
}

// 处理刷新按钮点击事件
void LoginWindow::onRefreshClicked()
{
    fetchVmList();
}

// 处理修改密码按钮点击事件
void LoginWindow::onChangePasswordClicked()
{
    if (m_token.isEmpty() || m_username.isEmpty()) {
        QMessageBox::warning(this, translate("alert_title"), translate("please_login_first"));
        return;
    }

    // 创建密码输入对话框
    QDialog dialog(this);
    dialog.setWindowTitle(translate("change_password"));
    dialog.setFixedSize(400, 200);

    QVBoxLayout *layout = new QVBoxLayout(&dialog);

    QLabel *label1 = new QLabel(translate("enter_new_password"));
    QLineEdit *passwordEdit1 = new QLineEdit();
    passwordEdit1->setEchoMode(QLineEdit::Password);

    QLabel *label2 = new QLabel(translate("enter_new_password_again"));
    QLineEdit *passwordEdit2 = new QLineEdit();
    passwordEdit2->setEchoMode(QLineEdit::Password);

    QHBoxLayout *buttonLayout = new QHBoxLayout();
    QPushButton *okButton = new QPushButton(translate("confirm"));
    QPushButton *cancelButton = new QPushButton(translate("cancel"));

    buttonLayout->addStretch();
    buttonLayout->addWidget(okButton);
    buttonLayout->addWidget(cancelButton);

    layout->addWidget(label1);
    layout->addWidget(passwordEdit1);
    layout->addWidget(label2);
    layout->addWidget(passwordEdit2);
    layout->addLayout(buttonLayout);

    connect(okButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);

    if (dialog.exec() == QDialog::Accepted) {
        QString password1 = passwordEdit1->text();
        QString password2 = passwordEdit2->text();

        if (password1.isEmpty()) {
            QMessageBox::warning(this, translate("alert_title"), translate("password_cannot_be_empty"));
            return;
        }

        if (password1 != password2) {
            QMessageBox::warning(this, translate("alert_title"), translate("passwords_do_not_match"));
            return;
        }

        // 发送修改密码请求
        QString urlStr = buildApiUrl("api/v1/users/password");

        QUrl url(urlStr);
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("Authorization", ("Bearer " + m_token).toUtf8());

        QSslConfiguration sslConfig = request.sslConfiguration();
        sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
        request.setSslConfiguration(sslConfig);

        QJsonObject json;
        json["username"] = m_username;
        json["newPassword"] = password1;

        QJsonDocument doc(json);
        QByteArray data = doc.toJson();

        QNetworkReply *reply = m_networkManager->put(request, data);
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            onChangePasswordReply(reply);
        });
    }
}

// 处理修改密码响应
void LoginWindow::onChangePasswordReply(QNetworkReply *reply)
{
    reply->deleteLater();

    if (isTokenExpired(reply)) {
        handleTokenExpired();
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        QMessageBox::warning(this, translate("change_password_failed"), translate("network_error_try_again"));
        return;
    }

    QByteArray responseData = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(responseData);

    if (doc.isNull() || !doc.isObject()) {
        QMessageBox::warning(this, translate("change_password_failed"), translate("server_response_error"));
        return;
    }

    QJsonObject json = doc.object();

    if (json.contains("code") && json["code"].toInt() == 0) {
        QMessageBox::information(this, translate("change_password_success"), translate("change_password_success"));
    } else {
        QString message = json.contains("message") ? json["message"].toString() : translate("change_password_failed");
        QMessageBox::warning(this, translate("change_password_failed"), message);
    }
}

// 获取虚拟机列表
void LoginWindow::fetchVmList()
{
    if (m_token.isEmpty()) {
        return;
    }

    QString urlStr = buildApiUrl("api/v1/users/" + m_username + "/vms");

    QUrl url(urlStr);
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", ("Bearer " + m_token).toUtf8());

    QSslConfiguration sslConfig = request.sslConfiguration();
    sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
    request.setSslConfiguration(sslConfig);

    m_vmListData.clear();

    QNetworkReply *reply = m_networkManager->get(request);
    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        QByteArray data = reply->readAll();
        m_vmListData.append(data);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onVmListReply(reply);
    });
}

// 处理虚拟机列表响应
void LoginWindow::onVmListReply(QNetworkReply *reply)
{
    reply->deleteLater();

    if (isTokenExpired(reply)) {
        handleTokenExpired();
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        m_vmStatusLabel->setText(translate("network_error"));
        m_vmStatusLabel->show();
        return;
    }

    QByteArray responseData = m_vmListData;
    QJsonDocument doc = QJsonDocument::fromJson(responseData);

    if (doc.isNull() || !doc.isObject()) {
        m_vmStatusLabel->setText(translate("server_error"));
        m_vmStatusLabel->show();
        return;
    }

    QJsonObject json = doc.object();

    if (!json.contains("code") || json["code"].toInt() != 0) {
        m_vmStatusLabel->setText(translate("server_error"));
        m_vmStatusLabel->show();
        return;
    }

    QJsonArray vmArray = json["data"].toArray();

    updateVmListWidget();

    QVBoxLayout *vmListLayout = qobject_cast<QVBoxLayout*>(m_vmListWidget->layout());
    if (!vmListLayout) {
        return;
    }

    m_vmStatusLabels.clear();
    m_vmStartButtons.clear();

    for (const QJsonValue &vmValue : vmArray) {
        QJsonObject vmObj = vmValue.toObject();
        QString vmId = QString::number(vmObj.value("vmid").toInt());
        QString vmName = vmObj.value("vm_name").toString();

        QFrame *vmFrame = createVmItemWidget(vmName, vmId, "");
        vmListLayout->addWidget(vmFrame);

        fetchVmStatus(vmId);
    }
}

// 处理虚拟机启动按钮点击事件
void LoginWindow::onVmStartClicked(const QString &vmId)
{
    if (m_token.isEmpty()) {
        return;
    }

    QPushButton *startButton = m_vmStartButtons.value(vmId);
    if (startButton) {
        startButton->setEnabled(false);
    }

    QString urlStr = buildApiUrl("api/v1/vm/" + vmId + "/start");

    QUrl url(urlStr);
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", ("Bearer " + m_token).toUtf8());

    QSslConfiguration sslConfig = request.sslConfiguration();
    sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
    request.setSslConfiguration(sslConfig);

    m_vmStartData[vmId].clear();

    QNetworkReply *reply = m_networkManager->post(request, QByteArray());

    QTimer *timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(5000);
    connect(timer, &QTimer::timeout, this, [this, reply, vmId, timer]() {
        if (reply && reply->isRunning()) {
            reply->abort();
        }
        QPushButton *startButton = m_vmStartButtons.value(vmId);
        if (startButton) {
            startButton->setEnabled(true);
        }
        timer->deleteLater();
    });

    connect(reply, &QNetworkReply::readyRead, this, [this, reply, vmId]() {
        QByteArray data = reply->readAll();
        m_vmStartData[vmId].append(data);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, vmId, timer]() {
        timer->stop();
        timer->deleteLater();
        onVmStartReply(reply, vmId);
    });

    timer->start();
}

// 处理虚拟机启动响应
void LoginWindow::onVmStartReply(QNetworkReply *reply, const QString &vmId)
{
    reply->deleteLater();

    if (isTokenExpired(reply)) {
        handleTokenExpired();
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        QPushButton *startButton = m_vmStartButtons.value(vmId);
        if (startButton) {
            startButton->setEnabled(true);
        }
        return;
    }

    QByteArray responseData = m_vmStartData[vmId];
    QJsonDocument doc = QJsonDocument::fromJson(responseData);

    if (doc.isNull() || !doc.isObject()) {
        QPushButton *startButton = m_vmStartButtons.value(vmId);
        if (startButton) {
            startButton->setEnabled(true);
        }
        return;
    }

    QJsonObject json = doc.object();

    if (json.contains("code") && json["code"].toInt() == 0) {
        updateVmStatus(vmId, "running", true);
    } else {
        QPushButton *startButton = m_vmStartButtons.value(vmId);
        if (startButton) {
            startButton->setEnabled(true);
        }
    }
}

// 处理虚拟机关机按钮点击事件
void LoginWindow::onVmStopClicked(const QString &vmId)
{
    QString url = buildApiUrl(QString("/api/v1/vm/%1/shutdown").arg(vmId));

    QNetworkRequest request(url);
    request.setRawHeader(QByteArray("Authorization"), QByteArray("Bearer " + m_token.toUtf8()));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArray("application/json"));

    QNetworkReply *reply = m_networkManager->post(request, QByteArray());
    connect(reply, &QNetworkReply::finished, this, [this, reply, vmId]() {
        onVmStopReply(reply, vmId);
    });
}

// 处理虚拟机关机响应
void LoginWindow::onVmStopReply(QNetworkReply *reply, const QString &vmId)
{
    reply->deleteLater();

    if (isTokenExpired(reply)) {
        handleTokenExpired();
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "onVmStopReply: error =" << reply->error() << reply->errorString();
        return;
    }

    QByteArray responseData = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(responseData);

    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "onVmStopReply: invalid JSON response";
        return;
    }

    QJsonObject json = doc.object();
    qInfo() << "onVmStopReply: json =" << json;
    if (json.contains("code") && json["code"].toInt() == 0) {
        qInfo() << "onVmStopReply: stop success";
        updateVmStatus(vmId, "stopped", true);
    }
}

// 处理虚拟机重启按钮点击事件
void LoginWindow::onVmRestartClicked(const QString &vmId)
{
    QString url = buildApiUrl(QString("/api/v1/vm/%1/restart").arg(vmId));

    QNetworkRequest request(url);
    request.setRawHeader(QByteArray("Authorization"), QByteArray("Bearer " + m_token.toUtf8()));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArray("application/json"));

    QNetworkReply *reply = m_networkManager->post(request, QByteArray());
    connect(reply, &QNetworkReply::finished, this, [this, reply, vmId]() {
        onQVmRestartReply(reply, vmId);
    });
}

// 处理虚拟机重启响应
void LoginWindow::onQVmRestartReply(QNetworkReply *reply, const QString &vmId)
{
    reply->deleteLater();

    if (isTokenExpired(reply)) {
        handleTokenExpired();
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "onQVmRestartReply: error =" << reply->error() << reply->errorString();
        return;
    }

    QByteArray responseData = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(responseData);

    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "onQVmRestartReply: invalid JSON response";
        return;
    }

    QJsonObject json = doc.object();
    qInfo() << "onQVmRestartReply: json =" << json;
    if (json.contains("code") && json["code"].toInt() == 0) {
        qInfo() << "onQVmRestartReply: restart success";
        updateVmStatus(vmId, "running", true);
    } else {
        qWarning() << "onQVmRestartReply: restart failed";
    }
}

// 处理虚拟机恢复按钮点击事件
void LoginWindow::onVmRestoreClicked(const QString &vmId)
{
    QString url = buildApiUrl(QString("/api/v1/vm/%1/rollback").arg(vmId));

    QNetworkRequest request(url);
    request.setRawHeader(QByteArray("Authorization"), QByteArray("Bearer " + m_token.toUtf8()));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArray("application/json"));

    QNetworkReply *reply = m_networkManager->post(request, QByteArray());
    connect(reply, &QNetworkReply::finished, this, [this, reply, vmId]() {
        onVmRestoreReply(reply, vmId);
    });
}

// 处理虚拟机还原响应
void LoginWindow::onVmRestoreReply(QNetworkReply *reply, const QString &vmId)
{
    reply->deleteLater();

    if (isTokenExpired(reply)) {
        handleTokenExpired();
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "onVmRestoreReply: error =" << reply->error() << reply->errorString();
        return;
    }

    QByteArray responseData = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(responseData);

    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "onVmRestoreReply: invalid JSON response";
        return;
    }

    QJsonObject json = doc.object();
    qInfo() << "onVmRestoreReply: json =" << json;
    if (json.contains("code") && json["code"].toInt() == 0) {
        qInfo() << "onVmRestoreReply: restore success";
        updateVmStatus(vmId, "stopped", true);
    }
}

// 处理虚拟机连接按钮点击事件
void LoginWindow::onVmConnectClicked(const QString &vmId)
{
    qInfo() << "onVmConnectClicked: vmId =" << vmId;
    QString url = buildApiUrl(QString("/api/v1/vm/%1/rdp").arg(vmId));
    qInfo() << "onVmConnectClicked: url =" << url;

    QNetworkRequest request(url);
    request.setRawHeader(QByteArray("Authorization"), QByteArray("Bearer " + m_token.toUtf8()));

    QNetworkReply *reply = m_networkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, vmId]() {
        onVmRdpFileReply(reply, vmId);
    });
}

// 处理RDP文件下载响应
void LoginWindow::onVmRdpFileReply(QNetworkReply *reply, const QString &vmId)
{
    reply->deleteLater();

    if (isTokenExpired(reply)) {
        handleTokenExpired();
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "onVmRdpFileReply: error =" << reply->error() << reply->errorString();
        QMessageBox::warning(this, translate("connect"), translate("connect_failed"));
        return;
    }

    QByteArray rdpData = reply->readAll();
    qInfo() << "onVmRdpFileReply: rdpData.size() =" << rdpData.size();

    // 保存RDP文件到用户应用数据目录
    QString appLocalAppData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);

    QDir appDataDir(appLocalAppData);
    if (!appDataDir.exists()) {
        appDataDir.mkpath(".");
    }

    QString rdpFilePath = appDataDir.filePath("template.rdp");

    QFile rdpFile(rdpFilePath);
    if (!rdpFile.open(QIODevice::WriteOnly)) {
        qWarning() << "onVmRdpFileReply: failed to open file for writing";
        QMessageBox::warning(this, translate("connect"), translate("connect_failed"));
        return;
    }

    qint64 bytesWritten = rdpFile.write(rdpData);
    if (bytesWritten != rdpData.size()) {
        qWarning() << "onVmRdpFileReply: failed to write all data, written =" << bytesWritten << "expected =" << rdpData.size();
        QMessageBox::warning(this, translate("connect"), translate("connect_failed"));
        return;
    }

    if (!rdpFile.flush()) {
        qWarning() << "onVmRdpFileReply: failed to flush file";
        QMessageBox::warning(this, translate("connect"), translate("connect_failed"));
        return;
    }

    rdpFile.close();

    if (!QFile::exists(rdpFilePath)) {
        qWarning() << "onVmRdpFileReply: file does not exist after writing";
        QMessageBox::warning(this, translate("connect"), translate("connect_failed"));
        return;
    }

    QFileInfo fileInfo(rdpFilePath);
    if (fileInfo.size() != rdpData.size()) {
        qWarning() << "onVmRdpFileReply: file size mismatch, actual =" << fileInfo.size() << "expected =" << rdpData.size();
        QMessageBox::warning(this, translate("connect"), translate("connect_failed"));
        return;
    }

    qInfo() << "onVmRdpFileReply: file saved successfully";

    // 获取连接命令
    QString url = buildApiUrl(QString("/api/v1/vm/%1/login").arg(vmId));
    qInfo() << "onVmRdpFileReply: login url =" << url;

    QNetworkRequest request(url);
    request.setRawHeader(QByteArray("Authorization"), QByteArray("Bearer " + m_token.toUtf8()));
    request.setRawHeader(QByteArray("Content-Type"), QByteArray("application/json"));

    QNetworkReply *loginReply = m_networkManager->get(request);
    connect(loginReply, &QNetworkReply::finished, this, [this, loginReply, vmId]() {
        onVmLoginReply(loginReply, vmId);
    });
}

// 处理获取连接命令响应
void LoginWindow::onVmLoginReply(QNetworkReply *reply, const QString &vmId)
{
    reply->deleteLater();

    qInfo() << "onVmLoginReply: vmId =" << vmId;

    if (isTokenExpired(reply)) {
        handleTokenExpired();
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "onVmLoginReply: error =" << reply->error() << reply->errorString();
        QMessageBox::warning(this, translate("connect"), translate("connect_failed"));
        return;
    }

    QByteArray responseData = reply->readAll();

    QJsonDocument doc = QJsonDocument::fromJson(responseData);

    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "onVmLoginReply: invalid JSON response";
        QMessageBox::warning(this, translate("connect"), translate("connect_failed"));
        return;
    }

    QJsonObject json = doc.object();
    qInfo() << "onVmLoginReply: json =" << json;

    if (json.contains("code") && json["code"].toInt() == 0) {
        QJsonObject data = json["data"].toObject();
        QString command = data.value("command").toString();

        if (command.isEmpty()) {
            qWarning() << "onVmLoginReply: command is empty";
            QMessageBox::warning(this, translate("connect"), translate("connect_failed"));
            return;
        }

        qInfo() << "onVmLoginReply: command =" << command;

        // 执行连接命令
        QString appDir = QCoreApplication::applicationDirPath();

        QDir dir(appDir);
        QString binDir;

        // 尝试在应用程序目录下查找bin目录
        if (dir.exists("bin")) {
            binDir = dir.absoluteFilePath("bin");
        } else {
            // 向上查找bin目录
            while (dir.cdUp()) {
                if (dir.exists("bin")) {
                    binDir = dir.absoluteFilePath("bin");
                    break;
                }
            }
        }

        // 如果还是找不到，就使用应用程序目录/bin
        if (binDir.isEmpty()) {
            binDir = appDir + "/bin";
        }

        // 使用用户应用数据目录中的RDP文件
        QString appLocalAppData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        QString rdpFilePath = QDir(appLocalAppData).filePath("template.rdp");

        QStringList args = command.split(" ", Qt::SkipEmptyParts);
        if (args.isEmpty()) {
            qWarning() << "onVmLoginReply: command split is empty";
            QMessageBox::warning(this, translate("connect"), translate("connect_failed"));
            return;
        }

        // 移除第一个参数（原始程序名，如 wfreerdp.exe 或 qf-client），
        // 替换为实际的 qf-client 路径
        args.removeFirst();
        QString programPath = binDir + "/qf-client";

        // 替换RDP文件路径为用户目录中的完整路径
        // 并展开环境变量（如 $HOME、$USER）
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (int i = 0; i < args.size(); ++i) {
            if (args[i].contains("template.rdp")) {
                args[i] = rdpFilePath;
            }
            // 展开 ~ 为用户 home 目录
            if (args[i].startsWith("~")) {
                args[i] = QDir::homePath() + args[i].mid(1);
            }
            // 展开 $VAR 和 ${VAR} 环境变量
            static QRegularExpression varRe("\\$\\{?([A-Za-z_][A-Za-z0-9_]*)\\}?");
            QRegularExpressionMatch match;
            int pos = 0;
            while ((match = varRe.match(args[i], pos)).hasMatch()) {
                QString varName = match.captured(1);
                QString varValue = env.value(varName);
                if (!varValue.isEmpty()) {
                    args[i].replace(match.capturedStart(), match.capturedLength(), varValue);
                    pos = match.capturedStart() + varValue.length();
                } else {
                    pos = match.capturedEnd();
                }
            }
        }

        if (m_rdpProcess) {
            if (m_rdpProcess->state() == QProcess::Running) {
                m_rdpProcess->terminate();
                if (!m_rdpProcess->waitForFinished(3000)) {
                    m_rdpProcess->kill();
                }
            }
            delete m_rdpProcess;
            m_rdpProcess = nullptr;
        }

        // 使用 /f（全屏）模式启动 qf-client，让 FreeRDP 自身处理分辨率
        // 避免 Qt DPR 检测问题导致分辨率参数错误
        args.append("/f");
        qInfo() << "onVmLoginReply: using /f fullscreen mode";

        m_rdpProcess = new QProcess(this);
        m_rdpProcess->setWorkingDirectory(binDir);

        connect(m_rdpProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &LoginWindow::onRdpProcessFinished);
        connect(m_rdpProcess, &QProcess::errorOccurred,
                this, &LoginWindow::onRdpProcessErrorOccurred);

        m_rdpProcess->start(programPath, args);

        qInfo() << "onVmLoginReply: starting qf-client process";
        qInfo() << "onVmLoginReply: programPath =" << programPath;
        qInfo() << "onVmLoginReply: arguments =" << args;
        qInfo() << "onVmLoginReply: workingDirectory =" << binDir;

        if (!m_rdpProcess->waitForStarted()) {
            qWarning() << "onVmLoginReply: failed to start process";
            qWarning() << "onVmLoginReply: error =" << m_rdpProcess->error();
            qWarning() << "onVmLoginReply: errorString =" << m_rdpProcess->errorString();
            QMessageBox::warning(this, translate("connect"), translate("connect_failed"));
        }
    } else {
        qWarning() << "onVmLoginReply: code =" << json["code"].toInt();
        QMessageBox::warning(this, translate("connect"), translate("connect_failed"));
    }
}

void LoginWindow::onRdpProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    qInfo() << "onRdpProcessFinished: exitCode =" << exitCode;
    qInfo() << "onRdpProcessFinished: exitStatus =" << exitStatus;

    if (m_rdpProcess) {
        qInfo() << "onRdpProcessFinished: stdout =" << m_rdpProcess->readAllStandardOutput();
        qInfo() << "onRdpProcessFinished: stderr =" << m_rdpProcess->readAllStandardError();
    }
}

void LoginWindow::onRdpProcessErrorOccurred(QProcess::ProcessError error)
{
    qWarning() << "onRdpProcessErrorOccurred: error =" << error;
    if (m_rdpProcess) {
        qWarning() << "onRdpProcessErrorOccurred: errorString =" << m_rdpProcess->errorString();
    }
}

void LoginWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
}

// 获取虚拟机状态
void LoginWindow::fetchVmStatus(const QString &vmId)
{
    if (m_token.isEmpty()) {
        return;
    }

    QString urlStr = buildApiUrl("api/v1/vm/" + vmId + "/currentstatus");

    QUrl url(urlStr);
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", ("Bearer " + m_token).toUtf8());

    QSslConfiguration sslConfig = request.sslConfiguration();
    sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
    request.setSslConfiguration(sslConfig);

    m_vmStatusData[vmId].clear();

    QNetworkReply *reply = m_networkManager->get(request);
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, vmId]() {
        QByteArray data = reply->readAll();
        m_vmStatusData[vmId].append(data);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, vmId]() {
        onVmStatusReply(reply, vmId);
    });
}

// 处理虚拟机状态响应
void LoginWindow::onVmStatusReply(QNetworkReply *reply, const QString &vmId)
{
    reply->deleteLater();

    if (isTokenExpired(reply)) {
        handleTokenExpired();
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        return;
    }

    QByteArray responseData = m_vmStatusData[vmId];
    QJsonDocument doc = QJsonDocument::fromJson(responseData);

    if (doc.isNull() || !doc.isObject()) {
        return;
    }

    QJsonObject json = doc.object();

    if (!json.contains("code") || json["code"].toInt() != 0) {
        return;
    }

    QJsonObject data = json["data"].toObject();
    QString status = data.value("status").toString();

    updateVmStatus(vmId, status);
}

// 更新虚拟机状态显示
void LoginWindow::updateVmStatus(const QString &vmId, const QString &status, bool delaySnapshot)
{
    QLabel *statusLabel = m_vmStatusLabels.value(vmId);
    if (!statusLabel) {
        return;
    }

    QString statusText;
    QString statusColor = "#64748b";

    if (status == "running") {
        statusText = translate("status_running");
        statusColor = "#10b981";
    } else if (status == "stopped") {
        statusText = translate("status_stopped");
        statusColor = "#ef4444";
    } else if (status == "paused") {
        statusText = translate("status_paused");
        statusColor = "#f59e0b";
    } else {
        statusText = translate("status_unknown");
        statusColor = "#dc3545";
    }

    statusLabel->setText(statusText);
    statusLabel->setStyleSheet(
        "font-size: 13px;"
        "color: " + statusColor + ";"
        "padding: 5px 10px;"
        "background-color: white;"
        "border-radius: 6px;"
        "border: 1px solid " + statusColor + ";"
    );

    if (delaySnapshot) {
        // 操作后延迟 2 分钟再查快照，给服务器端处理时间
        QTimer::singleShot(120000, this, [this, vmId]() {
            fetchVmSnapshot(vmId);
        });
    } else {
        fetchVmSnapshot(vmId);
    }
}

// 查询虚拟机快照
void LoginWindow::fetchVmSnapshot(const QString &vmId)
{
    if (m_token.isEmpty()) {
        return;
    }

    QString urlStr = buildApiUrl("api/v1/vm/" + vmId + "/hasmilestone");

    QUrl url(urlStr);
    QNetworkRequest request(url);
    request.setRawHeader("Authorization", ("Bearer " + m_token).toUtf8());

    QSslConfiguration sslConfig = request.sslConfiguration();
    sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
    request.setSslConfiguration(sslConfig);

    QNetworkReply *reply = m_networkManager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, vmId]() {
        onVmSnapshotReply(reply, vmId);
    });
}

// 处理虚拟机快照响应
void LoginWindow::onVmSnapshotReply(QNetworkReply *reply, const QString &vmId)
{
    reply->deleteLater();

    if (isTokenExpired(reply)) {
        handleTokenExpired();
        return;
    }

    QPushButton *restoreButton = m_vmRestoreButtons.value(vmId);
    if (!restoreButton) {
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        qInfo() << "onVmSnapshotReply: error =" << reply->error() << reply->errorString();
        restoreButton->setVisible(false);
        m_vmHasSnapshot[vmId] = false;
        return;
    }

    QByteArray responseData = reply->readAll();

    QJsonDocument doc = QJsonDocument::fromJson(responseData);

    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "onVmSnapshotReply: invalid JSON response";
        restoreButton->setVisible(false);
        m_vmHasSnapshot[vmId] = false;
        return;
    }

    QJsonObject json = doc.object();
    qInfo() << "onVmSnapshotReply: json =" << json;

    if (json.contains("code") && json["code"].toInt() == 0) {
        restoreButton->setVisible(true);
        m_vmHasSnapshot[vmId] = true;
    } else {
        restoreButton->setVisible(false);
        m_vmHasSnapshot[vmId] = false;
    }
}

// 初始化多语言翻译字典
void LoginWindow::initTranslations()
{
    m_translations["en_US"]["title"] = "VDI Login";
    m_translations["en_US"]["server"] = "Server";
    m_translations["en_US"]["username"] = "Username";
    m_translations["en_US"]["password"] = "Password";
    m_translations["en_US"]["remember_password"] = "Remember Password";
    m_translations["en_US"]["auto_login"] = "Auto Login";
    m_translations["en_US"]["login"] = "Login";
    m_translations["en_US"]["logging_in"] = "Logging in...";
    m_translations["en_US"]["network_error"] = "Network error, please check server address";
    m_translations["en_US"]["alert_title"] = "Alert";
    m_translations["en_US"]["enter_server_address"] = "Please enter server address";
    m_translations["en_US"]["enter_username"] = "Please enter username";
    m_translations["en_US"]["enter_password"] = "Please enter password";
    m_translations["en_US"]["success"] = "Success";
    m_translations["en_US"]["login_success"] = "Login successful!";
    m_translations["en_US"]["login_failed"] = "Login failed";
    m_translations["en_US"]["server_unreachable"] = "Server unreachable";
    m_translations["en_US"]["invalid_server_format"] = "Invalid server address format. Please enter domain or IP (e.g., vdi.example.com or 192.168.1.60)";
    m_translations["en_US"]["invalid_username_password"] = "Invalid username or password";
    m_translations["en_US"]["account_locked"] = "Account locked, please try again after 30 minutes";
    m_translations["en_US"]["token_expired"] = "Token expired, please login again";
    m_translations["en_US"]["vm_list_title"] = "Virtual Machine List";
    m_translations["en_US"]["back"] = "Back";
    m_translations["en_US"]["refresh"] = "Refresh";
    m_translations["en_US"]["server_error"] = "Server error";
    m_translations["en_US"]["start"] = "Start";
    m_translations["en_US"]["stop"] = "Shutdown";
    m_translations["en_US"]["restart"] = "Restart";
    m_translations["en_US"]["restore"] = "Restore";
    m_translations["en_US"]["connect"] = "Connect";
    m_translations["en_US"]["start_vm"] = "Start virtual machine";
    m_translations["en_US"]["stop_vm"] = "Stop virtual machine";
    m_translations["en_US"]["restart_vm"] = "Restart virtual machine";
    m_translations["en_US"]["restore_vm"] = "Restore virtual machine";
    m_translations["en_US"]["connect_vm"] = "Connect";
    m_translations["en_US"]["connect_failed"] = "Connection failed";
    m_translations["en_US"]["connect_success"] = "Connection successful";
    m_translations["en_US"]["disconnect"] = "Disconnect";
    m_translations["en_US"]["status_running"] = "Running";
    m_translations["en_US"]["status_stopped"] = "Stopped";
    m_translations["en_US"]["status_paused"] = "Paused";
    m_translations["en_US"]["status_unknown"] = "Unknown";
    m_translations["en_US"]["change_password"] = "Change Password";
    m_translations["en_US"]["enter_new_password"] = "Please enter new password:";
    m_translations["en_US"]["enter_new_password_again"] = "Please enter new password again:";
    m_translations["en_US"]["confirm"] = "Confirm";
    m_translations["en_US"]["cancel"] = "Cancel";
    m_translations["en_US"]["password_cannot_be_empty"] = "Password cannot be empty";
    m_translations["en_US"]["passwords_do_not_match"] = "Passwords do not match";
    m_translations["en_US"]["change_password_success"] = "Password changed successfully";
    m_translations["en_US"]["change_password_failed"] = "Failed to change password";
    m_translations["en_US"]["please_login_first"] = "Please login first";
    m_translations["en_US"]["network_error_try_again"] = "Network error, please try again later";
    m_translations["en_US"]["server_response_error"] = "Server response format error";

    m_translations["zh_CN"]["title"] = "VDI \xE7\x99\xBB\xE5\xBD\x95";
    m_translations["zh_CN"]["server"] = "\xE6\x9C\x8D\xE5\x8A\xA1\xE5\x99\xA8";
    m_translations["zh_CN"]["username"] = "\xE7\x94\xA8\xE6\x88\xB7\xE5\x90\x8D";
    m_translations["zh_CN"]["password"] = "\xE5\xAF\x86\xE7\xA0\x81";
    m_translations["zh_CN"]["remember_password"] = "\xE8\xAE\xB0\xE4\xBD\x8F\xE5\xAF\x86\xE7\xA0\x81";
    m_translations["zh_CN"]["auto_login"] = "\xE8\x87\xAA\xE5\x8A\xA8\xE7\x99\xBB\xE5\xBD\x95";
    m_translations["zh_CN"]["login"] = "\xE7\x99\xBB\xE5\xBD\x95";
    m_translations["zh_CN"]["logging_in"] = "\xE7\x99\xBB\xE5\xBD\x95\xE4\xB8\xAD...";
    m_translations["zh_CN"]["network_error"] = "\xE7\xBD\x91\xE7\xBB\x9C\xE9\x94\x99\xE8\xAF\xAF\xEF\xBC\x8C\xE8\xAF\xB7\xE6\xA3\x80\xE6\x9F\xA5\xE6\x9C\x8D\xE5\x8A\xA1\xE5\x99\xA8\xE5\x9C\xB0\xE5\x9D\x80";
    m_translations["zh_CN"]["alert_title"] = "\xE6\x8F\x90\xE7\xA4\xBA";
    m_translations["zh_CN"]["enter_server_address"] = "\xE8\xAF\xB7\xE8\xBE\x93\xE5\x85\xA5\xE6\x9C\x8D\xE5\x8A\xA1\xE5\x99\xA8\xE5\x9C\xB0\xE5\x9D\x80";
    m_translations["zh_CN"]["enter_username"] = "\xE8\xAF\xB7\xE8\xBE\x93\xE5\x85\xA5\xE7\x94\xA8\xE6\x88\xB7\xE5\x90\x8D";
    m_translations["zh_CN"]["enter_password"] = "\xE8\xAF\xB7\xE8\xBE\x93\xE5\x85\xA5\xE5\xAF\x86\xE7\xA0\x81";
    m_translations["zh_CN"]["success"] = "\xE6\x88\x90\xE5\x8A\x9F";
    m_translations["zh_CN"]["login_success"] = "\xE7\x99\xBB\xE5\xBD\x95\xE6\x88\x90\xE5\x8A\x9F\xEF\xBC\x81";
    m_translations["zh_CN"]["login_failed"] = "\xE7\x99\xBB\xE5\xBD\x95\xE5\xA4\xB1\xE8\xB4\xA5";
    m_translations["zh_CN"]["server_unreachable"] = "\xE6\x9C\x8D\xE5\x8A\xA1\xE5\x99\xA8\xE4\xB8\x8D\xE5\x8F\xAF\xE8\xBE\xBE";
    m_translations["zh_CN"]["invalid_server_format"] = "\xE6\x9C\x8D\xE5\x8A\xA1\xE5\x99\xA8\xE5\x9C\xB0\xE5\x9D\x80\xE6\xA0\xBC\xE5\xBC\x8F\xE9\x94\x99\xE8\xAF\xAF\xE3\x80\x82\xE8\xAF\xB7\xE8\xBE\x93\xE5\x85\xA5\xE5\x9F\x9F\xE5\x90\x8D\xE6\x88\x96IP\xE5\x9C\xB0\xE5\x9D\x80\xEF\xBC\x88\xE5\xA6\x82\xEF\xBC\x9Avdi.example.com \xE6\x88\x96 192.168.1.60\xEF\xBC\x89";
    m_translations["zh_CN"]["invalid_username_password"] = "\xE8\xBE\x93\xE5\x85\xA5\xE7\x9A\x84\xE7\x94\xA8\xE6\x88\xB7\xE5\x90\x8D\xE6\x88\x96\xE5\xAF\x86\xE7\xA0\x81\xE6\x9C\x89\xE8\xAF\xAF\xEF\xBC\x8C\xE8\xAF\xB7\xE9\x87\x8D\xE6\x96\xB0\xE8\xBE\x93\xE5\x85\xA5";
    m_translations["zh_CN"]["account_locked"] = "\xE8\xB4\xA6\xE6\x88\xB7\xE5\xB7\xB2\xE9\x94\x81\xE5\xAE\x9A\xEF\xBC\x8C\xE8\xAF\xB7 30 \xE5\x88\x86\xE9\x92\x9F\xE5\x90\x8E\xE5\x86\x8D\xE8\xAF\x95";
    m_translations["zh_CN"]["token_expired"] = "Token\xE5\xB7\xB2\xE8\xBF\x87\xE6\x9C\x9F\xEF\xBC\x8C\xE8\xAF\xB7\xE9\x87\x8D\xE6\x96\xB0\xE7\x99\xBB\xE5\xBD\x95";
    m_translations["zh_CN"]["vm_list_title"] = "\xE8\x99\x9A\xE6\x8B\x9F\xE6\x9C\xBA\xE5\x88\x97\xE8\xA1\xA8";
    m_translations["zh_CN"]["back"] = "\xE8\xBF\x94\xE5\x9B\x9E";
    m_translations["zh_CN"]["refresh"] = "\xE5\x88\xB7\xE6\x96\xB0";
    m_translations["zh_CN"]["server_error"] = "\xE6\x9C\x8D\xE5\x8A\xA1\xE5\x99\xA8\xE9\x94\x99\xE8\xAF\xAF";
    m_translations["zh_CN"]["start"] = "\xE5\xBC\x80\xE6\x9C\xBA";
    m_translations["zh_CN"]["stop"] = "\xE5\x85\xB3\xE6\x9C\xBA";
    m_translations["zh_CN"]["restart"] = "\xE9\x87\x8D\xE5\x90\xAF";
    m_translations["zh_CN"]["restore"] = "\xE8\xBF\x98\xE5\x8E\x9F";
    m_translations["zh_CN"]["start_vm"] = "\xE5\xBC\x80\xE6\x9C\xBA";
    m_translations["zh_CN"]["stop_vm"] = "\xE5\x85\xB3\xE6\x9C\xBA";
    m_translations["zh_CN"]["restart_vm"] = "\xE9\x87\x8D\xE5\x90\xAF";
    m_translations["zh_CN"]["restore_vm"] = "\xE8\xBF\x98\xE5\x8E\x9F";
    m_translations["zh_CN"]["connect"] = "\xE8\xBF\x9E\xE6\x8E\xA5";
    m_translations["zh_CN"]["connect_vm"] = "\xE8\xBF\x9E\xE6\x8E\xA5";
    m_translations["zh_CN"]["connect_failed"] = "\xE8\xBF\x9E\xE6\x8E\xA5\xE5\xA4\xB1\xE8\xB4\xA5";
    m_translations["zh_CN"]["connect_success"] = "\xE8\xBF\x9E\xE6\x8E\xA5\xE6\x88\x90\xE5\x8A\x9F";
    m_translations["zh_CN"]["disconnect"] = "\xE7\xBB\x93\xE6\x9D\x9F\xE8\xBF\x9E\xE6\x8E\xA5";
    m_translations["zh_CN"]["status_running"] = "\xE8\xBF\x90\xE8\xA1\x8C\xE4\xB8\xAD";
    m_translations["zh_CN"]["status_stopped"] = "\xE5\xB7\xB2\xE5\x85\xB3\xE6\x9C\xBA";
    m_translations["zh_CN"]["status_paused"] = "\xE5\xB7\xB2\xE6\x9A\x82\xE5\x81\x9C";
    m_translations["zh_CN"]["status_unknown"] = "\xE6\x9C\xAA\xE7\x9F\xA5";
    m_translations["zh_CN"]["change_password"] = "\xE4\xBF\xAE\xE6\x94\xB9\xE5\xAF\x86\xE7\xA0\x81";
    m_translations["zh_CN"]["enter_new_password"] = "\xE8\xAF\xB7\xE8\xBE\x93\xE5\x85\xA5\xE6\x96\xB0\xE5\xAF\x86\xE7\xA0\x81:";
    m_translations["zh_CN"]["enter_new_password_again"] = "\xE8\xAF\xB7\xE5\x86\x8D\xE6\xAC\xA1\xE8\xBE\x93\xE5\x85\xA5\xE6\x96\xB0\xE5\xAF\x86\xE7\xA0\x81:";
    m_translations["zh_CN"]["confirm"] = "\xE7\xA1\xAE\xE5\xAE\x9A";
    m_translations["zh_CN"]["cancel"] = "\xE5\x8F\x96\xE6\xB6\x88";
    m_translations["zh_CN"]["password_cannot_be_empty"] = "\xE5\xAF\x86\xE7\xA0\x81\xE4\xB8\x8D\xE8\x83\xBD\xE4\xB8\xBA\xE7\xA9\xBA";
    m_translations["zh_CN"]["passwords_do_not_match"] = "\xE4\xB8\xA4\xE6\xAC\xA1\xE8\xBE\x93\xE5\x85\xA5\xE7\x9A\x84\xE5\xAF\x86\xE7\xA0\x81\xE4\xB8\x8D\xE4\xB8\x80\xE8\x87\xB4";
    m_translations["zh_CN"]["change_password_success"] = "\xE4\xBF\xAE\xE6\x94\xB9\xE5\xAF\x86\xE7\xA0\x81\xE6\x88\x90\xE5\x8A\x9F";
    m_translations["zh_CN"]["change_password_failed"] = "\xE4\xBF\xAE\xE6\x94\xB9\xE5\xAF\x86\xE7\xA0\x81\xE5\xA4\xB1\xE8\xB4\xA5";
    m_translations["zh_CN"]["please_login_first"] = "\xE8\xAF\xB7\xE5\x85\x88\xE7\x99\xBB\xE5\xBD\x95";
    m_translations["zh_CN"]["network_error_try_again"] = "\xE7\xBD\x91\xE7\xBB\x9C\xE9\x94\x99\xE8\xAF\xAF\xEF\xBC\x8C\xE8\xAF\xB7\xE7\xA8\x8D\xE5\x90\x8E\xE9\x87\x8D\xE8\xAF\x95";
    m_translations["zh_CN"]["server_response_error"] = "\xE6\x9C\x8D\xE5\x8A\xA1\xE5\x99\xA8\xE5\x93\x8D\xE5\xBA\x94\xE6\xA0\xBC\xE5\xBC\x8F\xE9\x94\x99\xE8\xAF\xAF";

    m_translations["zh_TW"]["title"] = "VDI \xE7\x99\xBB\xE5\x85\xA5";
    m_translations["zh_TW"]["server"] = "\xE4\xBC\xBA\xE6\x9C\x8D\xE5\x99\xA8";
    m_translations["zh_TW"]["username"] = "\xE7\x94\xA8\xE6\x88\xB6\xE5\x90\x8D";
    m_translations["zh_TW"]["password"] = "\xE5\xAF\x86\xE7\xA2\xBC";
    m_translations["zh_TW"]["remember_password"] = "\xE8\xA8\x98\xE4\xBD\x8F\xE5\xAF\x86\xE7\xA2\xBC";
    m_translations["zh_TW"]["auto_login"] = "\xE8\x87\xAA\xE5\x8B\x95\xE7\x99\xBB\xE5\x85\xA5";
    m_translations["zh_TW"]["login"] = "\xE7\x99\xBB\xE5\x85\xA5";
    m_translations["zh_TW"]["logging_in"] = "\xE7\x99\xBB\xE5\x85\xA5\xE4\xB8\xAD...";
    m_translations["zh_TW"]["network_error"] = "\xE7\xB6\xB2\xE8\xB7\xAF\xE9\x8C\xAF\xE8\xAA\xA4\xEF\xBC\x8C\xE8\xAB\x8B\xE6\xAA\xA2\xE6\x9F\xA5\xE4\xBC\xBA\xE6\x9C\x8D\xE5\x99\xA8\xE5\x9C\xB0\xE5\x9D\x80";
    m_translations["zh_TW"]["alert_title"] = "\xE6\x8F\x90\xE7\xA4\xBA";
    m_translations["zh_TW"]["enter_server_address"] = "\xE8\xAB\x8B\xE8\xBC\xB8\xE5\x85\xA5\xE4\xBC\xBA\xE6\x9C\x8D\xE5\x99\xA8\xE5\x9C\xB0\xE5\x9D\x80";
    m_translations["zh_TW"]["enter_username"] = "\xE8\xAB\x8B\xE8\xBC\xB8\xE5\x85\xA5\xE7\x94\xA8\xE6\x88\xB6\xE5\x90\x8D";
    m_translations["zh_TW"]["enter_password"] = "\xE8\xAB\x8B\xE8\xBC\xB8\xE5\x85\xA5\xE5\xAF\x86\xE7\xA2\xBC";
    m_translations["zh_TW"]["success"] = "\xE6\x88\x90\xE5\x8A\x9F";
    m_translations["zh_TW"]["login_success"] = "\xE7\x99\xBB\xE5\x85\xA5\xE6\x88\x90\xE5\x8A\x9F\xEF\xBC\x81";
    m_translations["zh_TW"]["login_failed"] = "\xE7\x99\xBB\xE5\x85\xA5\xE5\xA4\xB1\xE6\x95\x97";
    m_translations["zh_TW"]["server_unreachable"] = "\xE4\xBC\xBA\xE6\x9C\x8D\xE5\x99\xA8\xE4\xB8\x8D\xE5\x8F\xAF\xE9\x81\x94";
    m_translations["zh_TW"]["invalid_server_format"] = "\xE4\xBC\xBA\xE6\x9C\x8D\xE5\x99\xA8\xE5\x9C\xB0\xE5\x9D\x80\xE6\xA0\xBC\xE5\xBC\x8F\xE9\x8C\xAF\xE8\xAA\xA4\xE3\x80\x82\xE8\xAB\x8B\xE8\xBC\xB8\xE5\x85\xA5\xE7\xB6\xB2\xE5\x9F\x9F\xE5\x90\x8D\xE7\xA8\xB1\xE6\x88\x96IP\xE4\xBD\x8D\xE5\x9D\x80\xEF\xBC\x88\xE5\xA6\x82\xEF\xBC\x9Avdi.example.com \xE6\x88\x96 192.168.1.60\xEF\xBC\x89";
    m_translations["zh_TW"]["invalid_username_password"] = "\xE8\xBC\xB8\xE5\x85\xA5\xE7\x9A\x84\xE7\x94\xA8\xE6\x88\xB6\xE5\x90\x8D\xE6\x88\x96\xE5\xAF\x86\xE7\xA2\xBC\xE6\x9C\x89\xE8\xAA\xA4\xEF\xBC\x8C\xE8\xAB\x8B\xE9\x87\x8D\xE6\x96\xB0\xE8\xBC\xB8\xE5\x85\xA5";
    m_translations["zh_TW"]["account_locked"] = "\xE8\xB4\x88\xE6\x88\xB6\xE5\xB7\xB2\xE9\x8E\x96\xE5\xAE\x9A\xEF\xBC\x8C\xE8\xAB\x8B 30 \xE5\x88\x86\xE9\x90\x98\xE5\xBE\x8C\xE5\x86\x8D\xE8\xA9\xA6";
    m_translations["zh_TW"]["token_expired"] = "Token\xE5\xB7\xB2\xE9\x81\x8E\xE6\x9C\x9F\xEF\xBC\x8C\xE8\xAB\x8B\xE9\x87\x8D\xE6\x96\xB0\xE7\x99\xBB\xE5\x85\xA5";
    m_translations["zh_TW"]["vm_list_title"] = "\xE8\x99\x9B\xE6\x93\xAC\xE6\xA9\x9F\xE5\x88\x97\xE8\xA1\xA8";
    m_translations["zh_TW"]["back"] = "\xE8\xBF\x94\xE5\x9B\x9E";
    m_translations["zh_TW"]["refresh"] = "\xE5\x88\xB7\xE6\x96\xB0";
    m_translations["zh_TW"]["server_error"] = "\xE4\xBC\xBA\xE6\x9C\x8D\xE5\x99\xA8\xE9\x8C\xAF\xE8\xAA\xA4";
    m_translations["zh_TW"]["start"] = "\xE9\x96\x8B\xE6\xA9\x9F";
    m_translations["zh_TW"]["stop"] = "\xE9\x97\x9C\xE6\xA9\x9F";
    m_translations["zh_TW"]["restart"] = "\xE9\x87\x8D\xE5\x95\x9F";
    m_translations["zh_TW"]["restore"] = "\xE9\x82\x84\xE5\x8E\x9F";
    m_translations["zh_TW"]["start_vm"] = "\xE9\x96\x8B\xE6\xA9\x9F\xE8\x99\x9B\xE6\x93\xAC\xE6\xA9\x9F";
    m_translations["zh_TW"]["stop_vm"] = "\xE9\x97\x9C\xE6\xA9\x9F\xE8\x99\x9B\xE6\x93\xAC\xE6\xA9\x9F";
    m_translations["zh_TW"]["restart_vm"] = "\xE9\x87\x8D\xE5\x95\x9F\xE8\x99\x9B\xE6\x93\xAC\xE6\xA9\x9F";
    m_translations["zh_TW"]["restore_vm"] = "\xE9\x82\x84\xE5\x8E\x9F\xE8\x99\x9B\xE6\x93\xAC\xE6\xA9\x9F";
    m_translations["zh_TW"]["connect"] = "\xE9\x80\xA3\xE6\x8E\xA5";
    m_translations["zh_TW"]["connect_vm"] = "\xE9\x80\xA3\xE6\x8E\xA5";
    m_translations["zh_TW"]["connect_failed"] = "\xE9\x80\xA3\xE6\x8E\xA5\xE5\xA4\xB1\xE6\x95\x97";
    m_translations["zh_TW"]["connect_success"] = "\xE9\x80\xA3\xE6\x8E\xA5\xE6\x88\x90\xE5\x8A\x9F";
    m_translations["zh_TW"]["disconnect"] = "\xE7\xB5\x90\xE6\x9D\x9F\xE9\x80\xA3\xE6\x8E\xA5";
    m_translations["zh_TW"]["status_running"] = "\xE9\x81\x8B\xE8\xA1\x8C\xE4\xB8\xAD";
    m_translations["zh_TW"]["status_stopped"] = "\xE5\xB7\xB2\xE9\x97\x9C\xE6\xA9\x9F";
    m_translations["zh_TW"]["status_paused"] = "\xE5\xB7\xB2\xE6\x9A\xAB\xE5\x81\x9C";
    m_translations["zh_TW"]["status_unknown"] = "\xE6\x9C\xAA\xE7\x9F\xA5";
    m_translations["zh_TW"]["change_password"] = "\xE4\xBF\xAE\xE6\x94\xB9\xE5\xAF\x86\xE7\xA2\xBC";
    m_translations["zh_TW"]["enter_new_password"] = "\xE8\xAB\x8B\xE8\xBC\xB8\xE5\x85\xA5\xE6\x96\xB0\xE5\xAF\x86\xE7\xA2\xBC:";
    m_translations["zh_TW"]["enter_new_password_again"] = "\xE8\xAB\x8B\xE5\x86\x8D\xE6\xAC\xA1\xE8\xBC\xB8\xE5\x85\xA5\xE6\x96\xB0\xE5\xAF\x86\xE7\xA2\xBC:";
    m_translations["zh_TW"]["confirm"] = "\xE7\xA2\xBA\xE5\xAE\x9A";
    m_translations["zh_TW"]["cancel"] = "\xE5\x8F\x96\xE6\xB6\x88";
    m_translations["zh_TW"]["password_cannot_be_empty"] = "\xE5\xAF\x86\xE7\xA2\xBC\xE4\xB8\x8D\xE8\x83\xBD\xE7\x82\xBA\xE7\xA9\xBA";
    m_translations["zh_TW"]["passwords_do_not_match"] = "\xE5\x85\xA9\xE6\xAC\xA1\xE8\xBC\xB8\xE5\x85\xA5\xE7\x9A\x84\xE5\xAF\x86\xE7\xA2\xBC\xE4\xB8\x8D\xE4\xB8\x80\xE8\x87\xB4";
    m_translations["zh_TW"]["change_password_success"] = "\xE4\xBF\xAE\xE6\x94\xB9\xE5\xAF\x86\xE7\xA2\xBC\xE6\x88\x90\xE5\x8A\x9F";
    m_translations["zh_TW"]["change_password_failed"] = "\xE4\xBF\xAE\xE6\x94\xB9\xE5\xAF\x86\xE7\xA2\xBC\xE5\xA4\xB1\xE6\x95\x97";
    m_translations["zh_TW"]["please_login_first"] = "\xE8\xAB\x8B\xE5\x85\x88\xE7\x99\xBB\xE5\x85\xA5";
    m_translations["zh_TW"]["network_error_try_again"] = "\xE7\xB6\xB2\xE8\xB7\xAF\xE9\x8C\xAF\xE8\xAA\xA4\xEF\xBC\x8C\xE8\xAB\x8B\xE7\xA8\x8D\xE5\xBE\x8C\xE9\x87\x8D\xE8\xA9\xA6";
    m_translations["zh_TW"]["server_response_error"] = "\xE4\xBC\xBA\xE6\x9C\x8D\xE5\x99\xA8\xE6\x87\x89\xE7\xAD\x94\xE6\xA0\xBC\xE5\xBC\x8F\xE9\x8C\xAF\xE8\xAA\xA4";

    m_translations["ja_JP"]["title"] = "VDI \xE3\x83\xAD\xE3\x82\xB0\xE3\x82\xA4\xE3\x83\xB3";
    m_translations["ja_JP"]["server"] = "\xE3\x82\xB5\xE3\x83\xBC\xE3\x83\x90\xE3\x83\xBC";
    m_translations["ja_JP"]["username"] = "\xE3\x83\xA6\xE3\x83\xBC\xE3\x82\xB6\xE3\x83\xBC\xE5\x90\x8D";
    m_translations["ja_JP"]["password"] = "\xE3\x83\x91\xE3\x82\xB9\xE3\x83\xAF\xE3\x83\xBC\xE3\x83\x89";
    m_translations["ja_JP"]["remember_password"] = "\xE3\x83\x91\xE3\x82\xB9\xE3\x83\xAF\xE3\x83\xBC\xE3\x83\x89\xE3\x82\x92\xE4\xBF\x9D\xE5\xAD\x98";
    m_translations["ja_JP"]["auto_login"] = "\xE8\x87\xAA\xE5\x8B\x95\xE3\x83\xAD\xE3\x82\xB0\xE3\x82\xA4\xE3\x83\xB3";
    m_translations["ja_JP"]["login"] = "\xE3\x83\xAD\xE3\x82\xB0\xE3\x82\xA4\xE3\x83\xB3";
    m_translations["ja_JP"]["logging_in"] = "\xE3\x83\xAD\xE3\x82\xB0\xE3\x82\xA4\xE3\x83\xB3\xE4\xB8\xAD...";
    m_translations["ja_JP"]["network_error"] = "\xE3\x83\x8D\xE3\x83\x83\xE3\x83\x88\xE3\x83\xAF\xE3\x83\xBC\xE3\x82\xAF\xE3\x82\xA8\xE3\x83\xA9\xE3\x83\xBC\xE3\x80\x81\xE3\x82\xB5\xE3\x83\xBC\xE3\x83\x90\xE3\x83\xBC\xE3\x82\xA2\xE3\x83\x89\xE3\x83\xAC\xE3\x82\xB9\xE3\x82\x92\xE7\xA2\xBA\xE8\xAA\x8D\xE3\x81\x97\xE3\x81\xA6\xE3\x81\x8F\xE3\x81\xA0\xE3\x81\x95\xE3\x81\x84";
    m_translations["ja_JP"]["alert_title"] = "\xE8\xAD\xA6\xE5\x91\x8A";
    m_translations["ja_JP"]["enter_server_address"] = "\xE3\x82\xB5\xE3\x83\xBC\xE3\x83\x90\xE3\x83\xBC\xE3\x82\xA2\xE3\x83\x89\xE3\x83\xAC\xE3\x82\xB9\xE3\x82\x92\xE5\x85\xA5\xE5\x8A\x9B\xE3\x81\x97\xE3\x81\xA6\xE3\x81\x8F\xE3\x81\xA0\xE3\x81\x95\xE3\x81\x84";
    m_translations["ja_JP"]["enter_username"] = "\xE3\x83\xA6\xE3\x83\xBC\xE3\x82\xB6\xE3\x83\xBC\xE5\x90\x8D\xE3\x82\x92\xE5\x85\xA5\xE5\x8A\x9B\xE3\x81\x97\xE3\x81\xA6\xE3\x81\x8F\xE3\x81\xA0\xE3\x81\x95\xE3\x81\x84";
    m_translations["ja_JP"]["enter_password"] = "\xE3\x83\x91\xE3\x82\xB9\xE3\x83\xAF\xE3\x83\xBC\xE3\x83\x89\xE3\x82\x92\xE5\x85\xA5\xE5\x8A\x9B\xE3\x81\x97\xE3\x81\xA6\xE3\x81\x8F\xE3\x81\xA0\xE3\x81\x95\xE3\x81\x84";
    m_translations["ja_JP"]["success"] = "\xE6\x88\x90\xE5\x8A\x9F";
    m_translations["ja_JP"]["login_success"] = "\xE3\x83\xAD\xE3\x82\xB0\xE3\x82\xA4\xE3\x83\xB3\xE6\x88\x90\xE5\x8A\x9F\xEF\xBC\x81";
    m_translations["ja_JP"]["login_failed"] = "\xE3\x83\xAD\xE3\x82\xB0\xE3\x82\xA4\xE3\x83\xB3\xE5\xA4\xB1\xE6\x95\x97";
    m_translations["ja_JP"]["server_unreachable"] = "\xE3\x82\xB5\xE3\x83\xBC\xE3\x83\x90\xE3\x83\xBC\xE3\x81\xAB\xE6\x8E\xA5\xE7\xB6\x9A\xE3\x81\xA7\xE3\x81\x8D\xE3\x81\xBE\xE3\x81\x9B\xE3\x82\x93";
    m_translations["ja_JP"]["invalid_server_format"] = "\xE3\x82\xB5\xE3\x83\xBC\xE3\x83\x90\xE3\x83\xBC\xE3\x82\xA2\xE3\x83\x89\xE3\x83\xAC\xE3\x82\xB9\xE3\x81\xAE\xE5\xBD\xA2\xE5\xBC\x8F\xE3\x81\x8C\xE7\x84\xA1\xE5\x8A\xB9\xE3\x81\xA7\xE3\x81\x99\xE3\x80\x82\xE3\x83\x89\xE3\x83\xA1\xE3\x82\xA4\xE3\x83\xB3\xE3\x81\xBE\xE3\x81\x9F\xE3\x81\xAFIP\xE3\x82\xA2\xE3\x83\x89\xE3\x83\xAC\xE3\x82\xB9\xE3\x82\x92\xE5\x85\xA5\xE5\x8A\x9B\xE3\x81\x97\xE3\x81\xA6\xE3\x81\x8F\xE3\x81\xA0\xE3\x81\x95\xE3\x81\x84\xEF\xBC\x88\xE4\xBE\x8B\xEF\xBC\x9Avdi.example.com \xE3\x81\xBE\xE3\x81\x9F\xE3\x81\xAF 192.168.1.60\xEF\xBC\x89";
    m_translations["ja_JP"]["invalid_username_password"] = "\xE3\x83\xA6\xE3\x83\xBC\xE3\x82\xB6\xE3\x83\xBC\xE5\x90\x8D\xE3\x81\xBE\xE3\x81\x9F\xE3\x81\xAF\xE3\x83\x91\xE3\x82\xB9\xE3\x83\xAF\xE3\x83\xBC\xE3\x83\x89\xE3\x81\x8C\xE9\x96\x93\xE9\x81\x95\xE3\x81\xA3\xE3\x81\xA6\xE3\x81\x84\xE3\x81\xBE\xE3\x81\x99";
    m_translations["ja_JP"]["account_locked"] = "\xE3\x82\xA2\xE3\x82\xAB\xE3\x82\xA6\xE3\x83\xB3\xE3\x83\x88\xE3\x81\x8C\xE3\x83\xAD\xE3\x83\x83\xE3\x82\xAF\xE3\x81\x95\xE3\x82\x8C\xE3\x81\xA6\xE3\x81\x84\xE3\x81\xBE\xE3\x81\x99\xE3\x80\x82" "30\xE5\x88\x86\xE5\xBE\x8C\xE3\x81\xAB\xE5\x86\x8D\xE8\xA9\xA6\xE8\xA1\x8C\xE3\x81\x97\xE3\x81\xA6\xE3\x81\x8F\xE3\x81\xA0\xE3\x81\x95\xE3\x81\x84";
    m_translations["ja_JP"]["token_expired"] = "\xE3\x83\x88\xE3\x83\xBC\xE3\x82\xAF\xE3\x83\xB3\xE3\x81\xAE\xE6\x9C\x89\xE5\x8A\xB9\xE6\x9C\x9F\xE9\x99\x90\xE3\x81\x8C\xE5\x88\x87\xE3\x82\x8C\xE3\x81\xBE\xE3\x81\x97\xE3\x81\x9F\xE3\x80\x82\xE5\x86\x8D\xE5\xBA\xA6\xE3\x83\xAD\xE3\x82\xB0\xE3\x82\xA4\xE3\x83\xB3\xE3\x81\x97\xE3\x81\xA6\xE3\x81\x8F\xE3\x81\xA0\xE3\x81\x95\xE3\x81\x84";
    m_translations["ja_JP"]["vm_list_title"] = "\xE4\xBB\xAE\xE6\x83\xB3\xE3\x83\x9E\xE3\x82\xB7\xE3\x83\xB3\xE4\xB8\x80\xE8\xA6\xA7";
    m_translations["ja_JP"]["back"] = "\xE6\x88\xBB\xE3\x82\x8B";
    m_translations["ja_JP"]["refresh"] = "\xE6\x9B\xB4\xE6\x96\xB0";
    m_translations["ja_JP"]["server_error"] = "\xE3\x82\xB5\xE3\x83\xBC\xE3\x83\x90\xE3\x83\xBC\xE3\x82\xA8\xE3\x83\xA9\xE3\x83\xBC";
    m_translations["ja_JP"]["start"] = "\xE8\xB5\xB7\xE5\x8B\x95";
    m_translations["ja_JP"]["stop"] = "\xE5\x81\x9C\xE6\xAD\xA2";
    m_translations["ja_JP"]["restart"] = "\xE5\x86\x8D\xE8\xB5\xB7\xE5\x8B\x95";
    m_translations["ja_JP"]["restore"] = "\xE5\xBE\xA9\xE5\x85\x83";
    m_translations["ja_JP"]["connect"] = "\xE6\x8E\xA5\xE7\xB6\x9A";
    m_translations["ja_JP"]["start_vm"] = "\xE4\xBB\xAE\xE6\x83\xB3\xE3\x83\x9E\xE3\x82\xB7\xE3\x83\xB3\xE3\x82\x92\xE8\xB5\xB7\xE5\x8B\x95";
    m_translations["ja_JP"]["stop_vm"] = "\xE4\xBB\xAE\xE6\x83\xB3\xE3\x83\x9E\xE3\x82\xB7\xE3\x83\xB3\xE3\x82\x92\xE5\x81\x9C\xE6\xAD\xA2";
    m_translations["ja_JP"]["restart_vm"] = "\xE4\xBB\xAE\xE6\x83\xB3\xE3\x83\x9E\xE3\x82\xB7\xE3\x83\xB3\xE3\x82\x92\xE5\x86\x8D\xE8\xB5\xB7\xE5\x8B\x95";
    m_translations["ja_JP"]["restore_vm"] = "\xE4\xBB\xAE\xE6\x83\xB3\xE3\x83\x9E\xE3\x82\xB7\xE3\x83\xB3\xE3\x82\x92\xE5\xBE\xA9\xE5\x85\x83";
    m_translations["ja_JP"]["connect_vm"] = "\xE6\x8E\xA5\xE7\xB6\x9A";
    m_translations["ja_JP"]["connect_failed"] = "\xE6\x8E\xA5\xE7\xB6\x9A\xE5\xA4\xB1\xE6\x95\x97";
    m_translations["ja_JP"]["connect_success"] = "\xE6\x8E\xA5\xE7\xB6\x9A\xE6\x88\x90\xE5\x8A\x9F";
    m_translations["ja_JP"]["disconnect"] = "\xE6\x8E\xA5\xE7\xB6\x9A\xE3\x82\x92\xE7\xB5\x82\xE4\xBA\x86";
    m_translations["ja_JP"]["status_running"] = "\xE5\xAE\x9F\xE8\xA1\x8C\xE4\xB8\xAD";
    m_translations["ja_JP"]["status_stopped"] = "\xE5\x81\x9C\xE6\xAD\xA2\xE4\xB8\xAD";
    m_translations["ja_JP"]["status_paused"] = "\xE4\xB8\x80\xE6\x99\x82\xE5\x81\x9C\xE6\xAD\xA2";
    m_translations["ja_JP"]["status_unknown"] = "\xE4\xB8\x8D\xE6\x98\x8E";
    m_translations["ja_JP"]["change_password"] = "\xE3\x83\x91\xE3\x82\xB9\xE3\x83\xAF\xE3\x83\xBC\xE3\x83\x89\xE3\x82\x92\xE5\xA4\x89\xE6\x9B\xB4";
    m_translations["ja_JP"]["enter_new_password"] = "\xE6\x96\xB0\xE3\x81\x97\xE3\x81\x84\xE3\x83\x91\xE3\x82\xB9\xE3\x83\xAF\xE3\x83\xBC\xE3\x83\x89\xE3\x82\x92\xE5\x85\xA5\xE5\x8A\x9B\xE3\x81\x97\xE3\x81\xA6\xE3\x81\x8F\xE3\x81\xA0\xE3\x81\x95\xE3\x81\x84:";
    m_translations["ja_JP"]["enter_new_password_again"] = "\xE6\x96\xB0\xE3\x81\x97\xE3\x81\x84\xE3\x83\x91\xE3\x82\xB9\xE3\x83\xAF\xE3\x83\xBC\xE3\x83\x89\xE3\x82\x92\xE5\x86\x8D\xE5\xBA\xA6\xE5\x85\xA5\xE5\x8A\x9B\xE3\x81\x97\xE3\x81\xA6\xE3\x81\x8F\xE3\x81\xA0\xE3\x81\x95\xE3\x81\x84:";
    m_translations["ja_JP"]["confirm"] = "\xE7\xA2\xBA\xE8\xAA\x8D";
    m_translations["ja_JP"]["cancel"] = "\xE3\x82\xAD\xE3\x83\xA3\xE3\x83\xB3\xE3\x82\xBB\xE3\x83\xAB";
    m_translations["ja_JP"]["password_cannot_be_empty"] = "\xE3\x83\x91\xE3\x82\xB9\xE3\x83\xAF\xE3\x83\xBC\xE3\x83\x89\xE3\x82\x92\xE5\x85\xA5\xE5\x8A\x9B\xE3\x81\x97\xE3\x81\xA6\xE3\x81\x8F\xE3\x81\xA0\xE3\x81\x95\xE3\x81\x84";
    m_translations["ja_JP"]["passwords_do_not_match"] = "\xE3\x83\x91\xE3\x82\xB9\xE3\x83\xAF\xE3\x83\xBC\xE3\x83\x89\xE3\x81\x8C\xE4\xB8\x80\xE8\x87\xB4\xE3\x81\x97\xE3\x81\xBE\xE3\x81\x9B\xE3\x82\x93";
    m_translations["ja_JP"]["change_password_success"] = "\xE3\x83\x91\xE3\x82\xB9\xE3\x83\xAF\xE3\x83\xBC\xE3\x83\x89\xE3\x81\xAE\xE5\xA4\x89\xE6\x9B\xB4\xE3\x81\xAB\xE6\x88\x90\xE5\x8A\x9F\xE3\x81\x97\xE3\x81\xBE\xE3\x81\x97\xE3\x81\x9F";
    m_translations["ja_JP"]["change_password_failed"] = "\xE3\x83\x91\xE3\x82\xB9\xE3\x83\xAF\xE3\x83\xBC\xE3\x83\x89\xE3\x81\xAE\xE5\xA4\x89\xE6\x9B\xB4\xE3\x81\xAB\xE5\xA4\xB1\xE6\x95\x97\xE3\x81\x97\xE3\x81\xBE\xE3\x81\x97\xE3\x81\x9F";
    m_translations["ja_JP"]["please_login_first"] = "\xE3\x81\xBE\xE3\x81\x9A\xE3\x83\xAD\xE3\x82\xB0\xE3\x82\xA4\xE3\x83\xB3\xE3\x81\x97\xE3\x81\xA6\xE3\x81\x8F\xE3\x81\xA0\xE3\x81\x95\xE3\x81\x84";
    m_translations["ja_JP"]["network_error_try_again"] = "\xE3\x83\x8D\xE3\x83\x83\xE3\x83\x88\xE3\x83\xAF\xE3\x83\xBC\xE3\x82\xAF\xE3\x82\xA8\xE3\x83\xA9\xE3\x83\xBC\xE3\x81\x8C\xE7\x99\xBA\xE7\x94\x9F\xE3\x81\x97\xE3\x81\xBE\xE3\x81\x97\xE3\x81\x9F\xE3\x80\x82\xE5\xBE\x8C\xE3\x81\xA7\xE3\x82\x82\xE3\x81\x86\xE4\xB8\x80\xE5\xBA\xA6\xE3\x81\x8A\xE8\xA9\xA6\xE3\x81\x97\xE3\x81\x8F\xE3\x81\xA0\xE3\x81\x95\xE3\x81\x84";
    m_translations["ja_JP"]["server_response_error"] = "\xE3\x82\xB5\xE3\x83\xBC\xE3\x83\x90\xE3\x83\xBC\xE3\x81\xAE\xE5\xBF\x9C\xE7\xAD\x94\xE5\xBD\xA2\xE5\xBC\x8F\xE3\x81\x8C\xE6\xAD\xA3\xE3\x81\x97\xE3\x81\x8F\xE3\x81\x82\xE3\x82\x8A\xE3\x81\xBE\xE3\x81\x9B\xE3\x82\x93";
}

// 更新应用语言
void LoginWindow::updateLanguage(const QString &languageCode)
{
    m_currentLanguage = languageCode;

    m_titleLabel->setText(translate("title"));
    m_serverEdit->setPlaceholderText(translate("server"));
    m_usernameEdit->setPlaceholderText(translate("username"));
    m_passwordEdit->setPlaceholderText(translate("password"));
    m_rememberPasswordCheckBox->setText(translate("remember_password"));
    m_autoLoginCheckBox->setText(translate("auto_login"));
    m_loginButton->setText(translate("login"));
    m_vmTitleLabel->setText(translate("vm_list_title"));
    m_backButton->setText(translate("back"));
    m_changePasswordButton->setText(translate("change_password"));
    m_refreshButton->setText(translate("refresh"));

    // 更新所有虚拟机连接按钮的文本
    for (auto it = m_vmConnectButtons.begin(); it != m_vmConnectButtons.end(); ++it) {
        if (it.value()) {
            it.value()->setText(translate("connect"));
        }
    }

    // 保存语言设置
    saveSettings();
}

// 翻译文本
QString LoginWindow::translate(const QString &key)
{
    if (m_translations.contains(m_currentLanguage) && m_translations[m_currentLanguage].contains(key)) {
        return m_translations[m_currentLanguage][key];
    }
    return key;
}

// 启动心跳检测
void LoginWindow::startHeartbeat()
{
    if (m_heartbeatTimer) {
        stopHeartbeat();
    }

    m_heartbeatTimer = new QTimer(this);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &LoginWindow::sendHeartbeat);
    m_heartbeatTimer->start(15000); // 15秒发送一次心跳

    // 立即发送一次心跳
    sendHeartbeat();
}

// 停止心跳检测
void LoginWindow::stopHeartbeat()
{
    if (m_heartbeatTimer) {
        m_heartbeatTimer->stop();
        delete m_heartbeatTimer;
        m_heartbeatTimer = nullptr;
    }
}

// 发送心跳请求
void LoginWindow::sendHeartbeat()
{
    if (m_token.isEmpty()) {
        return;
    }

    QString urlStr = buildApiUrl("api/v1/users/heartbeat");

    QUrl url(urlStr);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", ("Bearer " + m_token).toUtf8());

    QSslConfiguration sslConfig = request.sslConfiguration();
    sslConfig.setPeerVerifyMode(QSslSocket::VerifyNone);
    request.setSslConfiguration(sslConfig);

    QNetworkReply *reply = m_networkManager->post(request, QByteArray());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onHeartbeatReply(reply);
    });
}

// 处理心跳响应
void LoginWindow::onHeartbeatReply(QNetworkReply *reply)
{
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "Heartbeat error:" << reply->errorString();
        return;
    }

    QByteArray responseData = reply->readAll();
    QJsonDocument doc = QJsonDocument::fromJson(responseData);

    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "Invalid heartbeat response";
        return;
    }

    QJsonObject json = doc.object();
    if (json.contains("code") && json["code"].toInt() == 0) {
        qInfo() << "Heartbeat successful";
    } else {
        qWarning() << "Heartbeat failed:" << json["message"].toString();
    }
}

// 构建API URL
QString LoginWindow::buildApiUrl(const QString &path)
{
    QString server = m_server.trimmed();

    if (!server.startsWith("https://") && !server.startsWith("http://")) {
        server = "https://" + server;
    }

    QUrl url(server);
    int port = url.port();

    if (port == -1) {
        port = 443;
    }

    QString host = url.host();
    if (host.isEmpty()) {
        host = server;
        if (host.startsWith("https://")) {
            host = host.mid(8);
        } else if (host.startsWith("http://")) {
            host = host.mid(7);
        }
    }

    QString result = "https://" + host + ":" + QString::number(port);
    if (!path.startsWith("/")) {
        result += "/";
    }
    result += path;
    return result;
}

// 加载保存的设置
void LoginWindow::loadSettings()
{
    QSettings settings("VDIClient", "Login");

    QString server = settings.value("server", "").toString();
    QString username = settings.value("username", "").toString();
    QString password = settings.value("password", "").toString();
    bool rememberPassword = settings.value("rememberPassword", false).toBool();
    bool autoLogin = settings.value("autoLogin", false).toBool();
    QString language = settings.value("language", "en_US").toString();

    if (!server.isEmpty()) {
        m_serverEdit->setText(server);
    }
    if (!username.isEmpty()) {
        m_usernameEdit->setText(username);
    }
    if (!password.isEmpty() && rememberPassword) {
        m_passwordEdit->setText(password);
    }
    m_rememberPasswordCheckBox->setChecked(rememberPassword);
    m_autoLoginCheckBox->setChecked(autoLogin);

    if (!language.isEmpty()) {
        updateLanguage(language);
    }
}

// 保存设置
void LoginWindow::saveSettings()
{
    QSettings settings("VDIClient", "Login");

    QString server = m_serverEdit->text().trimmed();
    QString username = m_usernameEdit->text().trimmed();
    QString password = m_passwordEdit->text();
    bool rememberPassword = m_rememberPasswordCheckBox->isChecked();
    bool autoLogin = m_autoLoginCheckBox->isChecked();

    settings.setValue("server", server);
    settings.setValue("username", username);

    if (rememberPassword) {
        settings.setValue("password", password);
    } else {
        settings.remove("password");
    }

    settings.setValue("rememberPassword", rememberPassword);
    settings.setValue("autoLogin", autoLogin);
    settings.setValue("language", m_currentLanguage);
}

bool LoginWindow::isTokenExpired(QNetworkReply *reply)
{
    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    return statusCode == 401;
}

void LoginWindow::handleTokenExpired()
{
    qWarning() << "Token expired, returning to login page";
    m_token.clear();

    if (m_rdpProcess && m_rdpProcess->state() == QProcess::Running) {
        m_rdpProcess->terminate();
        if (!m_rdpProcess->waitForFinished(3000)) {
            m_rdpProcess->kill();
        }
    }

    m_stackedWidget->setCurrentIndex(0);

    if (m_statusLabel) {
        m_statusLabel->setText(translate("token_expired"));
        m_statusLabel->show();
    }

    if (m_loginButton) {
        m_loginButton->setEnabled(true);
        m_loginButton->setText(translate("login"));
    }
}
