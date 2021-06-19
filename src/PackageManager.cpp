#include "PackageManager.h"
#include "PythonInterpreter.h"

#include <QDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QRegExp>
#include <QTableWidgetItem>
#include <QTextCodec>
#include <QtGlobal>

#include <ccLog.h>

class CommandOutputDialog : public QDialog
{
    Q_OBJECT

  public:
    explicit CommandOutputDialog(QWidget *parent = nullptr)
        : QDialog(parent), m_display(new QPlainTextEdit(this))
    {
        setWindowTitle("pip output");
        m_display->setReadOnly(true);
        auto *widgetLayout = new QVBoxLayout;
        widgetLayout->addWidget(m_display);
        setLayout(widgetLayout);
        resize(600, 300);
    }

    void appendPlainText(const QString &text)
    {
        m_display->appendPlainText(text);
    }

    void clear()
    {
        m_display->clear();
    }

  private:
    QPlainTextEdit *m_display;
};

PackageManager::PackageManager(const PythonConfigPaths &config, QWidget *parent)
    : QWidget(parent), m_ui(new Ui_PackageManager), m_pythonProcess(new QProcess),
      m_outputDialog(new CommandOutputDialog(this))
{
    m_ui->setupUi(this);
    m_pythonProcess->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_pythonProcess, &QProcess::started, [this]() { setBusy(true); });
    connect(m_pythonProcess,
            static_cast<void (QProcess::*)(int)>(&QProcess::finished),
            [this](int) { setBusy(false); });

    m_ui->installedPackagesView->setColumnCount(2);
    m_ui->installedPackagesView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_ui->installedPackagesView->setHorizontalHeaderLabels({"Package Name", "Version"});
    m_ui->installedPackagesView->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    connect(m_ui->installedPackagesView,
            &QTableWidget::itemSelectionChanged,
            this,
            &PackageManager::handleSelectionChanged);

    m_ui->uninstallBtn->setEnabled(false);
    connect(m_ui->installBtn, &QPushButton::clicked, this, &PackageManager::handleInstallPackage);
    connect(
        m_ui->uninstallBtn, &QPushButton::clicked, this, &PackageManager::handleUninstallPackage);
    if (config.isSet())
    {

#ifdef Q_OS_WIN
        const QString pythonExePath = QString::fromWCharArray(config.pythonHome()) + "/python.exe";
#else
        const QString pythonExePath = QString::fromWCharArray(config.pythonHome()) + "/python";
#endif
        m_pythonProcess->setProgram(pythonExePath);
    }
    else
    {
        m_pythonProcess->setProgram("python");
    }
    refreshInstalledPackagesList();
}

void PackageManager::refreshInstalledPackagesList()
{
    const QStringList arguments = {"-m", "pip", "list"};
    m_pythonProcess->setArguments(arguments);

    QEventLoop loop;
    QObject::connect(
        m_pythonProcess,
        static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
        &loop,
        &QEventLoop::quit);
    m_pythonProcess->start(QIODevice::ReadOnly);
    loop.exec();

    if (m_pythonProcess->exitStatus() != QProcess::ExitStatus::NormalExit)
    {
        const QString errorMsg =
            QString("Failed to list installed packages: '%1'").arg(m_pythonProcess->errorString());
        QMessageBox::critical(this, "Package Manager Error", errorMsg);
        return;
    }

    const QString output =
        QTextCodec::codecForName("utf-8")->toUnicode(m_pythonProcess->readAllStandardOutput());

    const QVector<QStringRef> lines = output.splitRef("\n");

    const QRegExp regex(R"((\S*)(?:\s*)(\S*)(?:\s*)(\S?))");

    // First line is a header, second is separator
    // and last one seems to always be empty
    if (lines.size() <= 3)
    {
        return;
    }

    m_ui->installedPackagesView->setRowCount(lines.size() - 3);

    for (int i{2}; i < lines.size() - 1; ++i)
    {
        const QStringRef &currentLine = lines[i];

        // Do it this way to avoid an extra allocation
        int pos = regex.indexIn(currentLine.toString());
        if (pos != -1)
        {
            for (int j = 1; j < 3; ++j)
            {
                const QString lol = regex.cap(j);
                auto *thing = new QTableWidgetItem(lol);
                if (j != 1)
                {
                    thing->setFlags(thing->flags() & ~Qt::ItemFlag::ItemIsSelectable);
                }
                m_ui->installedPackagesView->setItem(i - 2, j - 1, thing);
            }
        }
    }
}

void PackageManager::handleInstallPackage()
{
    bool ok;
    const QString packageName = QInputDialog::getText(
        this, "Install Package", "package name", QLineEdit::Normal, QString(), &ok);

    if (!ok || packageName.isEmpty())
    {
        return;
    }

    const QStringList arguments = {"-m", "pip", "install", packageName};
    executeCommand(arguments);

    if (m_pythonProcess->exitCode() != 0)
    {
        ccLog::Error("Failed to run install commands", m_pythonProcess->error());
        ccLog::Warning(m_pythonProcess->errorString());
    }
    refreshInstalledPackagesList();
}

void PackageManager::handleUninstallPackage()
{
    const QList<QTableWidgetItem *> selectedItems = m_ui->installedPackagesView->selectedItems();

    if (selectedItems.isEmpty())
    {
        return;
    }

    for (const QTableWidgetItem *item : selectedItems)
    {
        const QString packageName = item->text();
        QMessageBox::StandardButton choice = QMessageBox::question(
            this, "Confirm", QString("Do you really want to uninstall: '%1' ?").arg(packageName));

        if (choice != QMessageBox::StandardButton::Yes)
        {
            continue;
        }
        const QStringList arguments = {"-m", "pip", "uninstall", "--yes", packageName};
        executeCommand(arguments);

        if (m_pythonProcess->exitCode() != 0)
        {
            ccLog::Error("Failed to run uninstall commands", m_pythonProcess->error());
            ccLog::Warning(m_pythonProcess->errorString());
        }
    }
    refreshInstalledPackagesList();
}

void PackageManager::executeCommand(const QStringList &arguments)
{
    m_outputDialog->show();
    m_outputDialog->clear();
    m_pythonProcess->setArguments(arguments);
    m_pythonProcess->start(QIODevice::ReadOnly);
    QTextCodec *utf8Codec = QTextCodec::codecForName("utf-8");

    while (m_pythonProcess->state() != QProcess::ProcessState::NotRunning)
    {
        if (m_pythonProcess->waitForReadyRead())
        {
            const QString output = utf8Codec->toUnicode(m_pythonProcess->readAll());
            m_outputDialog->appendPlainText(output);
            QApplication::processEvents();
        }
    }
    m_outputDialog->exec();
}

void PackageManager::handleSelectionChanged()
{
    m_ui->uninstallBtn->setEnabled(!m_ui->installedPackagesView->selectedItems().isEmpty());
}

void PackageManager::setBusy(bool isBusy)
{
    m_ui->installBtn->setEnabled(!isBusy);
    m_ui->uninstallBtn->setEnabled(!isBusy);
}

#include "PackageManager.moc"