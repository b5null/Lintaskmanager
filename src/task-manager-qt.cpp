#include "service-controller.h"
#include <QtCore/QProcess>
#include <QtCore/QElapsedTimer>
#include <QtCore/QSortFilterProxyModel>
#include <QtCore/QFileInfo>
#include <QtCore/QSignalBlocker>
#include <QtGui/QMouseEvent>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QScrollBar>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtGui/QCloseEvent>
#include <QtGui/QIcon>
#include <QtGui/QActionGroup>
#include <QtGui/QClipboard>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QStandardItem>
#include <QtGui/QStandardItemModel>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QListWidgetItem>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QFrame>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QStyleFactory>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QTableView>
#include <QtWidgets/QTreeView>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>
#include <QtWidgets/QGroupBox>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <functional>
#include <pwd.h>
#include <set>
#include <sstream>
#include <string>
#include <sys/utsname.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kRefreshMs = 1500;
constexpr int kMaxHistory = 120;
constexpr int RawPidRole = Qt::UserRole + 1;
constexpr int ParentPidRole = Qt::UserRole + 2;
constexpr int SortRole = Qt::UserRole + 3;
constexpr int WindowIdRole = Qt::UserRole + 4;

struct ProcessInfo {
    int pid = 0;
    int ppid = 0;
    std::string name;
    std::string status;
    std::string user;
    std::string command;
    unsigned long long ticks = 0;
    unsigned long long startTicks = 0;
    long residentPages = 0;
    int threads = 0;
    double cpu = 0.0;
};

std::string trim(const std::string &value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string readFile(const std::string &path) {
    std::ifstream input(path);
    if (!input) return {};
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

QString qs(const std::string &value) {
    return QString::fromLocal8Bit(value.c_str());
}

std::string currentUserName() {
    const passwd *account = getpwuid(getuid());
    return account && account->pw_name ? account->pw_name : "unknown";
}

QString formatBytes(unsigned long long bytes) {
    const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) return QString::number(static_cast<unsigned long long>(value)) + ' ' + units[unit];
    return QString::number(value, 'f', value >= 100.0 ? 0 : 1) + ' ' + units[unit];
}

QString formatRate(unsigned long long bytes) {
    return formatBytes(bytes) + "/s";
}

QString percentText(double value) {
    return QString::number(std::clamp(value, 0.0, 100.0), 'f', 1) + "%";
}

bool parseProcStat(const std::string &path, ProcessInfo &process) {
    std::ifstream input(path);
    if (!input) return false;
    std::string line;
    std::getline(input, line);
    const auto open = line.find('(');
    const auto close = line.rfind(") ");
    if (open == std::string::npos || close == std::string::npos || close <= open) return false;
    process.name = line.substr(open + 1, close - open - 1);
    std::istringstream fields(line.substr(close + 2));
    char state = '?';
    long long ignored = 0;
    if (!(fields >> state >> process.ppid)) return false;
    process.status = state == 'R' ? "Running" : state == 'S' ? "Sleeping" :
                     state == 'D' ? "Waiting" : state == 'T' ? "Stopped" :
                     state == 'Z' ? "Zombie" : "Unknown";
    for (int field = 5; field <= 13; ++field) if (!(fields >> ignored)) return false;
    unsigned long long userTicks = 0;
    unsigned long long systemTicks = 0;
    if (!(fields >> userTicks >> systemTicks)) return false;
    process.ticks = userTicks + systemTicks;
    for (int field = 16; field <= 19; ++field) if (!(fields >> ignored)) return false;
    return static_cast<bool>(fields >> process.threads >> ignored >> process.startTicks);
}

void readProcessDetails(ProcessInfo &process) {
    const std::string base = "/proc/" + std::to_string(process.pid);
    std::istringstream lines(readFile(base + "/status"));
    std::string line;
    while (std::getline(lines, line)) {
        if (line.rfind("Uid:", 0) == 0) {
            std::istringstream uidLine(line.substr(4));
            uid_t uid = 0;
            uidLine >> uid;
            if (const passwd *account = getpwuid(uid)) process.user = account->pw_name;
            else process.user = std::to_string(uid);
        } else if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream rssLine(line.substr(6));
            unsigned long long kib = 0;
            rssLine >> kib;
            process.residentPages = static_cast<long>(kib * 1024ULL /
                static_cast<unsigned long long>(getpagesize()));
        }
    }
    if (process.user.empty()) process.user = "unknown";
    std::string command = readFile(base + "/cmdline");
    for (char &character : command) if (character == '\0') character = ' ';
    process.command = trim(command);
    if (process.command.empty()) process.command = process.name;
}

std::vector<ProcessInfo> collectProcesses() {
    std::vector<ProcessInfo> result;
    DIR *directory = opendir("/proc");
    if (!directory) return result;
    while (dirent *entry = readdir(directory)) {
        char *end = nullptr;
        errno = 0;
        const long pid = std::strtol(entry->d_name, &end, 10);
        if (errno != 0 || end == entry->d_name || *end != '\0' || pid <= 0 || pid > INT32_MAX) continue;
        ProcessInfo process;
        process.pid = static_cast<int>(pid);
        if (parseProcStat("/proc/" + std::to_string(process.pid) + "/stat", process)) {
            readProcessDetails(process);
            result.push_back(std::move(process));
        }
    }
    closedir(directory);
    return result;
}

struct CpuTicks {
    unsigned long long total = 0;
    unsigned long long idle = 0;
};

std::vector<CpuTicks> readCpuCoreTicks() {
    std::ifstream input("/proc/stat");
    std::vector<CpuTicks> result;
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string label;
        if (!(fields >> label) || label.rfind("cpu", 0) != 0 || label == "cpu") continue;
        unsigned long long user = 0, nice = 0, system = 0, idle = 0, iowait = 0;
        unsigned long long irq = 0, softirq = 0, steal = 0;
        if (!(fields >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal)) continue;
        result.push_back({user + nice + system + idle + iowait + irq + softirq + steal,
                          idle + iowait});
    }
    return result;
}

std::map<std::string, std::pair<unsigned long long, unsigned long long>> readNetworkInterfaces() {
    std::ifstream input("/proc/net/dev");
    std::map<std::string, std::pair<unsigned long long, unsigned long long>> result;
    std::string line;
    std::getline(input, line);
    while (std::getline(input, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = trim(line.substr(0, colon));
        std::istringstream fields(line.substr(colon + 1));
        unsigned long long receive = 0, transmit = 0;
        if (!(fields >> receive)) continue;
        for (int i = 0; i < 7; ++i) {
            unsigned long long ignored = 0;
            fields >> ignored;
        }
        fields >> transmit;
        result[name] = {receive, transmit};
    }
    return result;
}

unsigned long long readTotalCpuTicks(unsigned long long &idleTicks) {
    std::ifstream input("/proc/stat");
    std::string label;
    unsigned long long user = 0, nice = 0, system = 0, idle = 0, iowait = 0;
    unsigned long long irq = 0, softirq = 0, steal = 0;
    if (!(input >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal)) {
        idleTicks = 0;
        return 0;
    }
    idleTicks = idle + iowait;
    return user + nice + system + idle + iowait + irq + softirq + steal;
}

unsigned long long readMemoryValue(const std::string &key) {
    std::ifstream input("/proc/meminfo");
    std::string name;
    unsigned long long value = 0;
    std::string unit;
    while (input >> name >> value >> unit) if (name == key) return value * 1024ULL;
    return 0;
}

unsigned long long readDiskBytes() {
    std::ifstream input("/proc/diskstats");
    unsigned int major = 0, minor = 0;
    std::string device;
    unsigned long long reads = 0, mergedReads = 0, sectorsRead = 0, readTime = 0;
    unsigned long long writes = 0, mergedWrites = 0, sectorsWritten = 0, writeTime = 0;
    unsigned long long total = 0;
    while (input >> major >> minor >> device >> reads >> mergedReads >> sectorsRead >> readTime
                 >> writes >> mergedWrites >> sectorsWritten >> writeTime) {
        std::string ignored;
        std::getline(input, ignored);
        if (device.rfind("loop", 0) == 0 || device.rfind("ram", 0) == 0) continue;
        if (!QFileInfo::exists(qs("/sys/block/" + device + "/device"))) continue;
        total += (sectorsRead + sectorsWritten) * 512ULL;
    }
    return total;
}

unsigned long long readNetworkBytes() {
    std::ifstream input("/proc/net/dev");
    std::string line;
    unsigned long long total = 0;
    std::getline(input, line);
    while (std::getline(input, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::istringstream fields(line.substr(colon + 1));
        unsigned long long receive = 0, transmit = 0;
        if (!(fields >> receive)) continue;
        for (int i = 0; i < 7; ++i) {
            unsigned long long ignored = 0;
            fields >> ignored;
        }
        fields >> transmit;
        total += receive + transmit;
    }
    return total;
}

unsigned long long counterRate(unsigned long long current, unsigned long long previous, double seconds) {
    return seconds > 0.0 && current >= previous
        ? static_cast<unsigned long long>((current - previous) / seconds) : 0;
}

struct MeterWidget final : QWidget {
    double value = 0.0;
    explicit MeterWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    QSize sizeHint() const override { return {48, 145}; }
    QSize minimumSizeHint() const override { return {0, 0}; }
    void setValue(double next) { value = std::clamp(next, 0.0, 100.0); update(); }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor("#050805"));
        painter.setPen(QColor(0, 170, 55, 95));
        for (int y = 10; y < height(); y += 7) painter.drawLine(1, y, width() - 2, y);
        const int filled = static_cast<int>((height() - 4) * value / 100.0);
        painter.fillRect(3, height() - 3 - filled, std::max(0, width() - 6), filled,
                        QColor("#00e83b"));
        painter.setPen(QColor("#667066"));
        painter.drawRect(0, 0, width() - 1, height() - 1);
    }
};

struct HistoryWidget final : QWidget {
    const std::vector<double> *history = nullptr;
    const std::vector<double> *secondaryHistory = nullptr;
    bool adaptiveScale = false;
    std::function<void()> onActivate;
    void makeInteractive(std::function<void()> callback, const QString &hint) {
        onActivate = std::move(callback);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setToolTip(hint);
        setAccessibleName(hint);
    }
    explicit HistoryWidget(const std::vector<double> *values, const std::vector<double> *secondary = nullptr,
                           QWidget *parent = nullptr, bool adaptive = false)
        : QWidget(parent), history(values), secondaryHistory(secondary), adaptiveScale(adaptive) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    QSize sizeHint() const override { return {220, 125}; }
    QSize minimumSizeHint() const override { return {0, 0}; }
protected:
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (onActivate && event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
            onActivate();
            event->accept();
        } else QWidget::mouseReleaseEvent(event);
    }
    void keyPressEvent(QKeyEvent *event) override {
        if (onActivate && !event->isAutoRepeat() &&
            (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
            onActivate();
            event->accept();
        } else QWidget::keyPressEvent(event);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor("#050805"));
        painter.setPen(QColor(0, 175, 80, 130));
        for (int x = 12; x < width(); x += 20) painter.drawLine(x, 0, x, height());
        for (int y = 10; y < height(); y += 20) painter.drawLine(0, y, width(), y);
        if (!history || history->empty()) return;
        double maximum = 100.0;
        if (adaptiveScale) {
            maximum = 0.0;
            const auto findMaximum = [&](const std::vector<double> *values) {
                if (!values) return;
                for (const double value : *values) maximum = std::max(maximum, value);
            };
            findMaximum(history);
            findMaximum(secondaryHistory);
            maximum = std::max(1024.0, maximum * 1.15);
        }
        const auto drawHistory = [&](const std::vector<double> *values, const QColor &color) {
            if (!values || values->empty()) return;
            QPainterPath path;
            for (size_t i = 0; i < values->size(); ++i) {
                const double x = (width() - 1.0) * static_cast<double>(kMaxHistory - values->size() + i) /
                                 static_cast<double>(kMaxHistory - 1);
                const double normalized = adaptiveScale
                    ? std::clamp((*values)[i] / maximum, 0.0, 1.0)
                    : std::clamp((*values)[i], 0.0, 100.0) / 100.0;
                const double y = height() - 2.0 - (height() - 4.0) * normalized;
                if (i == 0) path.moveTo(x, y); else path.lineTo(x, y);
            }
            painter.setPen(QPen(color, 2));
            painter.drawPath(path);
        };
        drawHistory(history, QColor("#29ed4b"));
        drawHistory(secondaryHistory, QColor("#f1db3b"));
        painter.setPen(QColor("#b9d9bd"));
        painter.drawText(rect().adjusted(6, 3, -6, -3), Qt::AlignTop | Qt::AlignRight,
                         adaptiveScale ? formatRate(static_cast<unsigned long long>(maximum)) : "100%");
        painter.setPen(hasFocus() ? QPen(Qt::white, 1, Qt::DashLine) : QPen(QColor("#667066")));
        painter.drawRect(0, 0, width() - 1, height() - 1);
    }
};

QGroupBox *group(const QString &title) {
    auto *box = new QGroupBox(title);
    box->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    return box;
}

QLabel *valueLabel(const QString &text = "Waiting for data...") {
    auto *label = new QLabel(text);
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    return label;
}

struct NetworkCard final : QFrame {
    QLabel *state = nullptr;
    QLabel *receive = nullptr;
    QLabel *transmit = nullptr;
    QLabel *receivedTotal = nullptr;
    QLabel *transmittedTotal = nullptr;
    HistoryWidget *graph = nullptr;
    QString copyText;
    unsigned long long receivedBytes = 0, transmittedBytes = 0, receiveRate = 0, transmitRate = 0;
    NetworkCard(const QString &name, const std::vector<double> *rx, const std::vector<double> *tx) {
        setObjectName("adapterCard");
        setProperty("adapterName", name);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(14, 12, 14, 12);
        layout->setSpacing(9);
        auto *header = new QHBoxLayout;
        auto *icon = new QLabel;
        icon->setPixmap(style()->standardIcon(QStyle::SP_DriveNetIcon).pixmap(24, 24));
        header->addWidget(icon);
        auto *title = new QLabel(name);
        title->setObjectName("adapterName");
        title->setTextFormat(Qt::PlainText);
        header->addWidget(title, 1);
        state = new QLabel;
        state->setObjectName("adapterState");
        state->setToolTip("Interface operational state; this does not indicate Internet connectivity.");
        header->addWidget(state);
        layout->addLayout(header);
        auto *rates = new QHBoxLayout;
        auto addRate = [&](const QString &label, QLabel **rate, QLabel **total, const QString &id) {
            auto *column = new QVBoxLayout;
            auto *caption = new QLabel(label);
            caption->setObjectName(id);
            column->addWidget(caption);
            *rate = new QLabel("0 B/s");
            (*rate)->setObjectName("adapterRate");
            column->addWidget(*rate);
            *total = new QLabel;
            (*total)->setObjectName("adapterDetail");
            column->addWidget(*total);
            rates->addLayout(column, 1);
        };
        addRate("↓ Receive", &receive, &receivedTotal, "receiveCaption");
        addRate("↑ Transmit", &transmit, &transmittedTotal, "transmitCaption");
        layout->addLayout(rates);
        graph = new HistoryWidget(rx, tx, this, true);
        graph->setFixedHeight(88);
        layout->addWidget(graph);
        auto *footer = new QHBoxLayout;
        auto *legend = new QLabel("Green: receive · Yellow: transmit");
        legend->setObjectName("adapterDetail");
        footer->addWidget(legend, 1);
        auto *copy = new QPushButton("Copy");
        copy->setObjectName("adapterCopy");
        copy->setToolTip("Copy this adapter's rates, totals and state");
        connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(copyText); });
        footer->addWidget(copy);
        layout->addLayout(footer);
    }
    void setCounters(const QString &name, const QString &operstate, unsigned long long rx,
                     unsigned long long tx, unsigned long long rxRate, unsigned long long txRate) {
        receivedBytes = rx;
        transmittedBytes = tx;
        receiveRate = rxRate;
        transmitRate = txRate;
        state->setText(operstate.isEmpty() ? "unknown" : operstate);
        receive->setText(formatRate(rxRate));
        transmit->setText(formatRate(txRate));
        receivedTotal->setText(formatBytes(rx) + " received");
        transmittedTotal->setText(formatBytes(tx) + " sent");
        copyText = QString("%1\nState: %2\nReceive: %3\nTransmit: %4\nReceived: %5\nSent: %6")
            .arg(name, state->text(), receive->text(), transmit->text(), formatBytes(rx), formatBytes(tx));
        graph->update();
    }
};

class TaskManagerWindow final : public QMainWindow {
    friend class TaskManagerTests;
public:
    TaskManagerWindow() {
        setWindowTitle("Task Manager");
        loadSettings();
        resize(820, 570);
        setMinimumSize(0, 0);
        if (!savedGeometry.isEmpty()) restoreGeometry(savedGeometry);
        if (alwaysOnTopEnabled) setWindowFlag(Qt::WindowStaysOnTopHint, true);
        buildMenus();
        buildPages();
        restoreViewSettings();
        applyTheme(themeName);
        if (tabs) tabs->setCurrentIndex(savedTab);
        connect(tabs, &QTabWidget::currentChanged, this, [this] { saveSettings(); });
        refreshAll();
        timer.setInterval(refreshMs);
        connect(&timer, &QTimer::timeout, this, &TaskManagerWindow::refreshAll);
        timer.start();
        updateRefreshStatus();
    }

    QSize minimumSizeHint() const override { return {0, 0}; }

private:
    QTabWidget *tabs = nullptr;
    QTreeView *processView = nullptr;
    QStandardItemModel *processModel = nullptr;
    QSortFilterProxyModel *processProxy = nullptr;
    QLineEdit *processSearch = nullptr;
    bool processTree = true;
    bool renderedProcessTree = true;
    int refreshMs = kRefreshMs;
    QElapsedTimer sampleClock;
    double sampleSeconds = 0.0;
    QLabel *refreshStatus = nullptr;
    bool windowsPending = false;
    bool servicesPending = false;
    ServiceController serviceController;
    std::map<QString, QAction *> serviceActions;
    QLabel *serviceOperationStatus = nullptr;
    bool serviceConfirming = false;
    unsigned long long serviceGeneration = 0;
    QString pendingService;
    QString pendingServiceVerb;
    QTreeView *userView = nullptr;
    QStandardItemModel *userModel = nullptr;
    QScrollArea *networkScroll = nullptr;
    QString networkLayout = "classic";
    QAction *networkClassicAction = nullptr;
    QAction *networkCardsAction = nullptr;
    QStackedWidget *networkLayouts = nullptr;
    QScrollArea *networkClassicScroll = nullptr;
    QGridLayout *networkClassicGraphsLayout = nullptr;
    QTreeView *networkList = nullptr;
    QStandardItemModel *networkListModel = nullptr;
    std::map<std::string, QGroupBox *> networkClassicGraphs;
    QLabel *networkSummary = nullptr;
    QLabel *networkEmpty = nullptr;
    bool showLoopback = false;
    QString selectedAdapter;
    QListWidget *applicationList = nullptr;
    QListWidget *serviceList = nullptr;
    QWidget *coreGraphsHost = nullptr;
    HistoryWidget *totalCpuGraph = nullptr;
    QScrollArea *coreScroll = nullptr;
    QAction *perCoreAction = nullptr;
    QLabel *cpuViewHint = nullptr;
    std::vector<QGroupBox *> corePanels;
    std::vector<HistoryWidget *> coreGraphs;
    bool perCoreGraphs = false;
    QGridLayout *coreGraphsLayout = nullptr;
    QWidget *networkGraphsHost = nullptr;
    QGridLayout *networkGraphsLayout = nullptr;
    QLabel *processCount = nullptr;
    QLabel *applicationCount = nullptr;
    QLabel *serviceCount = nullptr;
    QLabel *userCount = nullptr;
    QLabel *statusText = nullptr;
    QLabel *overviewCpu = nullptr;
    QLabel *overviewMemory = nullptr;
    QLabel *overviewSwap = nullptr;
    QLabel *overviewProcesses = nullptr;
    QLabel *overviewDisk = nullptr;
    QLabel *overviewNetwork = nullptr;
    QLabel *overviewUptime = nullptr;
    QLabel *performanceCpu = nullptr;
    QLabel *performanceMemory = nullptr;
    QLabel *performanceSwap = nullptr;
    QLabel *performanceCpuTotal = nullptr;
    QLabel *performanceMemoryTotal = nullptr;
    QLabel *performanceProcessCount = nullptr;
    QLabel *performanceThreadCount = nullptr;
    QLabel *performanceAvailableMemory = nullptr;
    QLabel *performanceCommit = nullptr;
    QLabel *performanceKernel = nullptr;
    MeterWidget *cpuMeter = nullptr;
    MeterWidget *memoryMeter = nullptr;
    MeterWidget *swapMeter = nullptr;
    std::vector<double> cpuHistory;
    std::vector<double> memoryHistory;
    std::vector<double> swapHistory;
    std::vector<double> diskHistory;
    std::vector<double> networkHistory;
    std::vector<std::vector<double>> coreHistories;
    std::map<std::string, std::vector<double>> networkReceiveHistories;
    std::map<std::string, std::vector<double>> networkTransmitHistories;
    std::map<std::string, NetworkCard *> networkCards;
    std::vector<ProcessInfo> processes;
    std::map<int, std::pair<unsigned long long, unsigned long long>> previousProcessTicks;
    unsigned long long previousTotalTicks = 0;
    unsigned long long previousIdleTicks = 0;
    unsigned long long previousDiskBytes = 0;
    unsigned long long previousNetworkBytes = 0;
    std::vector<CpuTicks> previousCoreTicks;
    std::map<std::string, std::pair<unsigned long long, unsigned long long>> previousNetworkInterfaces;
    std::set<int> collapsedProcesses;
    std::set<QString> collapsedUsers;
    bool showUserProcesses = false;
    bool restoringProcessTree = false;
    bool restoringUserTree = false;
    QString themeName = "xp";
    bool alwaysOnTopEnabled = false;
    int savedTab = 0;
    QByteArray savedGeometry;
    QTimer timer;

    void closeEvent(QCloseEvent *event) override {
        if (serviceController.busy()) {
            QMessageBox::information(this, "Service operation pending",
                "Finish or cancel the administrator authorization dialog and wait for the service command to finish before closing.");
            event->ignore();
            return;
        }
        saveSettings();
        QMainWindow::closeEvent(event);
    }

    static void appendHistory(std::vector<double> &history, double value) {
        history.push_back(value);
        if (static_cast<int>(history.size()) > kMaxHistory) history.erase(history.begin());
    }

    void loadSettings() {
        QSettings settings;
        const QString storedTheme = settings.value("appearance/theme", "xp").toString();
        const QStringList themes = {"xp", "light", "dark", "contrast"};
        themeName = themes.contains(storedTheme) ? storedTheme : QString("xp");
        alwaysOnTopEnabled = settings.value("appearance/alwaysOnTop", false).toBool();
        showUserProcesses = settings.value("view/showUserProcesses", false).toBool();
        networkLayout = settings.value("view/networkLayout", "classic").toString();
        if (networkLayout != "classic" && networkLayout != "cards") networkLayout = "classic";
        showLoopback = settings.value("view/showLoopback", false).toBool();
        perCoreGraphs = settings.value("view/perCoreGraphs", false).toBool();
        processTree = settings.value("view/processTree", true).toBool();
        refreshMs = settings.value("monitor/refreshMs", kRefreshMs).toInt();
        if (refreshMs != 500 && refreshMs != 1500 && refreshMs != 3000) refreshMs = kRefreshMs;
        savedTab = std::max(0, settings.value("window/tab", 0).toInt());
        savedGeometry = settings.value("window/geometry").toByteArray();

        for (const QString &pidText : settings.value("view/collapsedProcesses").toStringList()) {
            bool ok = false;
            const int pid = pidText.toInt(&ok);
            if (ok && pid > 0) collapsedProcesses.insert(pid);
        }
        for (const QString &user : settings.value("view/collapsedUsers").toStringList())
            if (!user.isEmpty()) collapsedUsers.insert(user);
    }

    void restoreViewSettings() {
        QSettings settings;
        if (processView && settings.contains("view/processHeader"))
            processView->header()->restoreState(settings.value("view/processHeader").toByteArray());
        if (userView && settings.contains("view/userHeader"))
            userView->header()->restoreState(settings.value("view/userHeader").toByteArray());

    }

    void rememberProcessExpansion() {
        if (!processModel || !processView || processModel->rowCount() == 0 || !processTree || (processSearch && !processSearch->text().isEmpty())) return;
        std::set<int> currentCollapsed;
        std::function<void(const QModelIndex &)> collect = [&](const QModelIndex &parent) {
            for (int row = 0; row < processModel->rowCount(parent); ++row) {
                const QModelIndex index = processModel->index(row, 0, parent);
                if (processModel->hasChildren(index) && !processView->isExpanded(processProxy->mapFromSource(index)))
                    currentCollapsed.insert(index.data(RawPidRole).toInt());
                collect(index);
            }
        };
        collect({});
        collapsedProcesses = std::move(currentCollapsed);
    }

    void saveSettings() {
        rememberProcessExpansion();
        rememberUserExpansion();
        QSettings settings;
        settings.setValue("appearance/theme", themeName);
        settings.setValue("appearance/alwaysOnTop", alwaysOnTopEnabled);
        settings.setValue("view/showUserProcesses", showUserProcesses);
        settings.setValue("view/processTree", processTree);
        settings.setValue("view/perCoreGraphs", perCoreGraphs);
        settings.setValue("monitor/refreshMs", refreshMs);
        settings.setValue("window/tab", tabs ? tabs->currentIndex() : savedTab);
        settings.setValue("window/geometry", saveGeometry());
        QStringList processRows;
        for (const int pid : collapsedProcesses) processRows << QString::number(pid);
        settings.setValue("view/collapsedProcesses", processRows);
        QStringList userRows;
        for (const QString &user : collapsedUsers) userRows << user;
        settings.setValue("view/collapsedUsers", userRows);
        if (processView) settings.setValue("view/processHeader", processView->header()->saveState());
        if (userView) settings.setValue("view/userHeader", userView->header()->saveState());
        settings.setValue("view/showLoopback", showLoopback);
        settings.setValue("view/networkLayout", networkLayout);
        settings.sync();
    }

    void updateRefreshStatus() {
        if (refreshStatus) refreshStatus->setText(timer.isActive()
            ? QString("Live · %1 s").arg(refreshMs / 1000.0, 0, 'f', 1) : "Paused · F5 to sample");
    }

    void buildMenus() {
        auto *file = menuBar()->addMenu("&File");
        auto *run = file->addAction("Run New Task...");
        file->addSeparator();
        file->addAction("Exit", this, &QWidget::close);
        connect(run, &QAction::triggered, this, [this] {
            bool accepted = false;
            const QString command = QInputDialog::getText(this, "Run New Task", "Open:",
                                                           QLineEdit::Normal, {}, &accepted);
            if (accepted && !command.trimmed().isEmpty()) QProcess::startDetached("sh", {"-c", command});
        });

        auto *options = menuBar()->addMenu("&Options");
        auto *themes = options->addMenu("Theme");
        auto *themeGroup = new QActionGroup(this);
        themeGroup->setExclusive(true);
        const QList<QPair<QString, QString>> themeNames = {
            {"XP Classic", "xp"}, {"Modern Light", "light"}, {"Modern Dark", "dark"},
            {"High Contrast", "contrast"}
        };
        for (const auto &[label, id] : themeNames) {
            auto *action = themes->addAction(label);
            action->setCheckable(true);
            action->setData(id);
            themeGroup->addAction(action);
            connect(action, &QAction::triggered, this, [this, action] { applyTheme(action->data().toString()); });
            if (id == themeName) action->setChecked(true);
        }
        options->addSeparator();
        auto *alwaysOnTop = options->addAction("Always on Top");
        alwaysOnTop->setCheckable(true);
        alwaysOnTop->setChecked(alwaysOnTopEnabled);
        connect(alwaysOnTop, &QAction::toggled, this, [this](bool checked) {
            alwaysOnTopEnabled = checked;
            setWindowFlag(Qt::WindowStaysOnTopHint, checked);
            show();
            saveSettings();
        });

        auto *view = menuBar()->addMenu("&View");
        view->addAction("Refresh Now", QKeySequence(Qt::Key_F5), this, &TaskManagerWindow::refreshAll);
        auto *pause = view->addAction("Pause updates");
        pause->setObjectName("pauseUpdates");
        pause->setCheckable(true);
        pause->setShortcut(QKeySequence("Ctrl+P"));
        connect(pause, &QAction::toggled, this, [this](bool paused) {
            if (paused) timer.stop(); else timer.start(refreshMs);
            updateRefreshStatus();
        });
        auto *speed = view->addMenu("Update speed");
        auto *speeds = new QActionGroup(this);
        for (const auto &[label, milliseconds] : QList<QPair<QString, int>>{
                 {"High (0.5 seconds)", 500}, {"Normal (1.5 seconds)", 1500}, {"Low (3 seconds)", 3000}}) {
            auto *action = speed->addAction(label);
            action->setCheckable(true);
            action->setChecked(refreshMs == milliseconds);
            speeds->addAction(action);
            connect(action, &QAction::triggered, this, [this, milliseconds] {
                refreshMs = milliseconds;
                timer.setInterval(refreshMs);
                updateRefreshStatus();
                saveSettings();
            });
        }
        perCoreAction = view->addAction("One CPU graph per logical processor");
        perCoreAction->setCheckable(true);
        perCoreAction->setChecked(perCoreGraphs);
        connect(perCoreAction, &QAction::toggled, this, &TaskManagerWindow::setCpuView);
        auto *networkViews = view->addMenu("Networking layout");
        auto *networkViewGroup = new QActionGroup(this);
        networkClassicAction = networkViews->addAction("Graphs + list");
        networkCardsAction = networkViews->addAction("Adapter cards");
        for (auto *action : {networkClassicAction, networkCardsAction}) {
            action->setCheckable(true);
            networkViewGroup->addAction(action);
        }
        networkClassicAction->setChecked(networkLayout == "classic");
        networkCardsAction->setChecked(networkLayout == "cards");
        connect(networkClassicAction, &QAction::triggered, this, [this] { setNetworkLayout("classic"); });
        connect(networkCardsAction, &QAction::triggered, this, [this] { setNetworkLayout("cards"); });
        auto *userProcesses = view->addAction("Show processes under Users");
        userProcesses->setCheckable(true);
        userProcesses->setChecked(showUserProcesses);
        connect(userProcesses, &QAction::toggled, this, [this](bool enabled) {
            showUserProcesses = enabled;
            refreshUsers();
            saveSettings();
        });
        auto *help = menuBar()->addMenu("&Help");
        help->addAction("About Task Manager", this, [this] {
            QMessageBox::about(this, "Task Manager",
                "A lightweight, open-source Linux task manager with a classic desktop layout.");
        });
    }

    void installContextMenu(QWidget *widget, int tabIndex) {
        if (!widget) return;
        widget->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(widget, &QWidget::customContextMenuRequested, this, [this, widget, tabIndex](const QPoint &point) {
            // Scroll-area context-menu signals use viewport coordinates.
            // Mapping them through the frame shifts the target up by the header height.
            auto *area = qobject_cast<QAbstractScrollArea *>(widget);
            QWidget *origin = area ? area->viewport() : widget;
            showTabContextMenu(tabIndex, origin->mapToGlobal(point));
        });
    }

    void selectContextItem(int tabIndex, const QPoint &globalPosition) {
        if (tabIndex == 0 && applicationList) {
            if (auto *item = applicationList->itemAt(applicationList->viewport()->mapFromGlobal(globalPosition)))
                applicationList->setCurrentItem(item);
            else applicationList->setCurrentRow(-1);
        } else if (tabIndex == 2 && serviceList) {
            serviceList->setCurrentItem(serviceList->itemAt(serviceList->viewport()->mapFromGlobal(globalPosition)));
        } else if (tabIndex == 4) {
            selectedAdapter.clear();
            if (networkLayout == "classic") {
                const auto index = networkList->indexAt(networkList->viewport()->mapFromGlobal(globalPosition));
                networkList->setCurrentIndex(index);
                if (index.isValid()) selectedAdapter = index.siblingAtColumn(0).data().toString();
                for (const auto &[name, graph] : networkClassicGraphs)
                    if (graph->isVisible() && graph->rect().contains(graph->mapFromGlobal(globalPosition))) selectedAdapter = qs(name);
            } else {
                for (const auto &[name, card] : networkCards)
                    if (card->isVisible() && card->rect().contains(card->mapFromGlobal(globalPosition))) selectedAdapter = qs(name);
            }
        } else if (tabIndex == 1 && processView) {
            const QModelIndex index = processView->indexAt(processView->viewport()->mapFromGlobal(globalPosition));
            if (index.isValid()) processView->selectionModel()->setCurrentIndex(
                index, processView->selectionModel()->isSelected(index) ? QItemSelectionModel::NoUpdate
                    : QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            else processView->setCurrentIndex({});
        } else if (tabIndex == 5 && userView) {
            const QModelIndex index = userView->indexAt(userView->viewport()->mapFromGlobal(globalPosition));
            if (index.isValid()) userView->selectionModel()->setCurrentIndex(
                index, userView->selectionModel()->isSelected(index) ? QItemSelectionModel::NoUpdate
                    : QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            else userView->setCurrentIndex({});
        }
    }

    QString selectedText(int tabIndex) const {
        if (tabIndex == 3) return {};
        if (tabIndex == 4) {
            const auto card = networkCards.find(selectedAdapter.toStdString());
            return card == networkCards.end() ? QString() : card->second->copyText;
        }
        if (tabIndex == 0 && applicationList && applicationList->currentItem())
            return applicationList->currentItem()->text();
        if (tabIndex == 2 && serviceList && serviceList->currentItem())
            return serviceList->currentItem()->text();
        QAbstractItemView *view = tabIndex == 1 ? static_cast<QAbstractItemView *>(processView)
            : static_cast<QAbstractItemView *>(userView);
        if (!view || !view->currentIndex().isValid()) return {};
        const QModelIndex index = view->currentIndex();
        QStringList values;
        for (int column = 0; column < index.model()->columnCount(index.parent()); ++column)
            values << index.siblingAtColumn(column).data().toString();
        return values.join("\t");
    }

    void runNewTaskDialog() {
        bool accepted = false;
        const QString command = QInputDialog::getText(this, "Run New Task", "Open:",
                                                       QLineEdit::Normal, {}, &accepted);
        if (accepted && !command.trimmed().isEmpty()) QProcess::startDetached("sh", {"-c", command});
    }

    void endProcessByPid(int pid, const QString &name, const QString &dialogTitle) {
        if (pid <= 0) return;
        if (QMessageBox::question(this, dialogTitle,
                                  QString("End %1 (PID %2)?").arg(name).arg(pid)) != QMessageBox::Yes)
            return;
        errno = 0;
        if (::kill(pid, SIGTERM) != 0) {
            const QString reason = QString::fromLocal8Bit(std::strerror(errno));
            QMessageBox::warning(this, dialogTitle,
                                  QString("Could not end process %1 (PID %2): %3")
                                      .arg(name).arg(pid).arg(reason));
        }
    }

    void switchToApplication() {
        if (!applicationList || !applicationList->currentItem()) return;
        const QString id = applicationList->currentItem()->data(WindowIdRole).toString();
        if (id.isEmpty()) {
            statusBar()->showMessage("Window activation is unavailable for this session process.", 5000);
            return;
        }
        runDiscovery("wmctrl", {"-i", "-a", id}, [this](bool ok, const QString &, const QString &error) {
            if (!ok) statusBar()->showMessage("Could not activate window: " + error, 5000);
        });
    }

    void endApplicationTask() {
        if (!applicationList || !applicationList->currentItem()) return;
        auto *item = applicationList->currentItem();
        const int pid = item->data(Qt::UserRole).toInt();
        endProcessByPid(pid, item->text(), "End Task");
    }

    std::map<int, QString> selectedProcesses(QTreeView *view) const {
        std::map<int, QString> selected;
        for (const auto &index : view->selectionModel()->selectedRows()) {
            if (view == userView && !index.parent().isValid()) continue;
            bool ok = false;
            const int pid = index.data(RawPidRole).toInt(&ok);
            if (ok && pid > 0) selected.emplace(pid, index.data().toString());
        }
        return selected;
    }

    void endProcesses(QTreeView *view) {
        // Snapshot before opening the dialog: refreshes must not change its targets.
        const auto selected = selectedProcesses(view);
        if (selected.empty()) return;
        QStringList names;
        for (const auto &[pid, name] : selected)
            names << QString("%1 (PID %2)").arg(name).arg(pid);
        QMessageBox confirm(QMessageBox::Question, "End Processes",
            QString("End %1 selected process(es)?").arg(selected.size()),
            QMessageBox::Yes | QMessageBox::No, this);
        confirm.setDefaultButton(QMessageBox::No);
        confirm.setDetailedText(names.join("\n"));
        if (confirm.exec() != QMessageBox::Yes) return;
        QStringList failures;
        for (const auto &[pid, name] : selected) {
            if (::kill(pid, SIGTERM) != 0)
                failures << QString("%1 (PID %2): %3").arg(name).arg(pid)
                    .arg(QString::fromLocal8Bit(std::strerror(errno)));
        }
        if (!failures.isEmpty())
            QMessageBox::warning(this, "End Processes", failures.join("\n"));
    }

    void endSelectedProcess() { endProcesses(processView); }
    void endSelectedUserProcess() { endProcesses(userView); }

    struct ViewState {
        QString anchor, current;
        QStringList selected;
        int offset = 0, vertical = 0, horizontal = 0;
    };

    QString rowKey(QTreeView *view, const QModelIndex &index) const {
        if (!index.isValid()) return {};
        return (view == userView && !index.parent().isValid() ? "user:" : "pid:")
            + index.siblingAtColumn(0).data(RawPidRole).toString();
    }

    ViewState rememberView(QTreeView *view) const {
        ViewState state;
        const auto top = view->indexAt(QPoint(view->viewport()->width() / 2, 0));
        state.anchor = rowKey(view, top);
        state.offset = view->visualRect(top).top();
        state.current = rowKey(view, view->currentIndex());
        for (const auto &index : view->selectionModel()->selectedRows())
            state.selected << rowKey(view, index);
        state.vertical = view->verticalScrollBar()->value();
        state.horizontal = view->horizontalScrollBar()->value();
        return state;
    }

    void restoreView(QTreeView *view, const ViewState &state) {
        std::map<QString, QModelIndex> rows;
        std::function<void(const QModelIndex &)> visit = [&](const QModelIndex &parent) {
            for (int row = 0; row < view->model()->rowCount(parent); ++row) {
                const auto index = view->model()->index(row, 0, parent);
                rows[rowKey(view, index)] = index;
                visit(index);
            }
        };
        visit({});
        view->selectionModel()->clearSelection();
        for (const auto &key : state.selected) {
            const auto found = rows.find(key);
            if (found != rows.end()) view->selectionModel()->select(found->second,
                QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
        const auto current = rows.find(state.current);
        view->selectionModel()->setCurrentIndex(current == rows.end() ? QModelIndex() : current->second,
                                                QItemSelectionModel::NoUpdate);
        view->doItemsLayout();
        const auto anchor = rows.find(state.anchor);
        if (anchor != rows.end()) {
            view->scrollTo(anchor->second, QAbstractItemView::PositionAtTop);
            view->verticalScrollBar()->setValue(view->verticalScrollBar()->value() - state.offset);
        } else view->verticalScrollBar()->setValue(state.vertical);
        view->horizontalScrollBar()->setValue(state.horizontal);
    }

    void clearHistory() {
        cpuHistory.clear();
        memoryHistory.clear();
        swapHistory.clear();
        diskHistory.clear();
        networkHistory.clear();
        for (auto &history : coreHistories) history.clear();
        for (auto &[name, history] : networkReceiveHistories) history.clear();
        for (auto &[name, history] : networkTransmitHistories) history.clear();
        for (QWidget *widget : findChildren<QWidget *>()) widget->update();
    }

    void showTabContextMenu(int tabIndex, const QPoint &globalPosition) {
        selectContextItem(tabIndex, globalPosition);
        QMenu menu(this);
        menu.addAction("Refresh", this, &TaskManagerWindow::refreshAll);
        menu.addSeparator();
        if (tabIndex == 0) {
            menu.addAction("New Task...", this, &TaskManagerWindow::runNewTaskDialog);
            auto *end = menu.addAction("End Task", this, &TaskManagerWindow::endApplicationTask);
            end->setEnabled(applicationList && applicationList->currentItem());
            auto *switchTo = menu.addAction("Switch To", this, &TaskManagerWindow::switchToApplication);
            switchTo->setEnabled(applicationList && applicationList->currentItem() &&
                !applicationList->currentItem()->data(WindowIdRole).toString().isEmpty());
        } else if (tabIndex == 1) {
            auto *end = menu.addAction("End Selected Processes", this, &TaskManagerWindow::endSelectedProcess);
            end->setEnabled(!selectedProcesses(processView).empty());
            menu.addSeparator();
            menu.addAction("Expand All", processView, &QTreeView::expandAll);
            menu.addAction("Collapse All", processView, &QTreeView::collapseAll);
            auto *copy = menu.addAction("Copy selected process");
            copy->setEnabled(processView && processView->currentIndex().isValid());
            connect(copy, &QAction::triggered, this, [this] {
                QApplication::clipboard()->setText(selectedText(1));
            });
        } else if (tabIndex == 2) {
            updateServiceActions();
            for (const auto &verb : {"start", "stop", "restart", "reload"}) menu.addAction(serviceActions.at(verb));
            menu.addSeparator();
            auto *copy = menu.addAction("Copy selected service");
            copy->setEnabled(serviceList && serviceList->currentItem());
            connect(copy, &QAction::triggered, this, [this] {
                QApplication::clipboard()->setText(selectedText(2));
            });
        } else if (tabIndex == 3) {
            menu.addAction("Clear graph history", this, &TaskManagerWindow::clearHistory);
        } else if (tabIndex == 4) {
            auto *copy = menu.addAction("Copy selected adapter statistics");
            copy->setEnabled(!selectedText(4).isEmpty());
            connect(copy, &QAction::triggered, this, [this] {
                QApplication::clipboard()->setText(selectedText(4));
            });
            menu.addAction("Clear graph history", this, &TaskManagerWindow::clearHistory);
        } else if (tabIndex == 5) {
            auto *children = menu.addAction("Show processes under Users");
            children->setCheckable(true);
            children->setChecked(showUserProcesses);
            connect(children, &QAction::toggled, this, [this](bool enabled) {
                showUserProcesses = enabled;
                refreshUsers();
                saveSettings();
            });
            menu.addAction("Expand All", userView, &QTreeView::expandAll);
            menu.addAction("Collapse All", userView, &QTreeView::collapseAll);
            auto *end = menu.addAction("End Selected Processes", this, &TaskManagerWindow::endSelectedUserProcess);
            end->setEnabled(!selectedProcesses(userView).empty());
            auto *copy = menu.addAction("Copy selected user");
            copy->setEnabled(userView && userView->currentIndex().isValid());
            connect(copy, &QAction::triggered, this, [this] {
                QApplication::clipboard()->setText(selectedText(5));
            });
        }
        const QString selection = selectedText(tabIndex);
        if (tabIndex != 1 && tabIndex != 2 && tabIndex != 4 && tabIndex != 5) {
            auto *copy = menu.addAction("Copy selection");
            copy->setEnabled(!selection.isEmpty());
            connect(copy, &QAction::triggered, this, [this, tabIndex] {
                QApplication::clipboard()->setText(selectedText(tabIndex));
            });
        }
        menu.exec(globalPosition);
    }

    void installContextMenus() {
        if (!tabs) return;
        tabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(tabs->tabBar(), &QTabBar::customContextMenuRequested, this, [this](const QPoint &point) {
            const int index = tabs->tabBar()->tabAt(point);
            if (index >= 0) showTabContextMenu(index, tabs->tabBar()->mapToGlobal(point));
        });
        for (int index = 0; index < tabs->count(); ++index) {
            QWidget *page = tabs->widget(index);
            installContextMenu(page, index);
            for (QWidget *child : page->findChildren<QWidget *>()) installContextMenu(child, index);
        }
    }

    QWidget *pageWithTable(const QString &title, const QString &subtitle, QTableView *view, QLabel **count,
                           const QString &initialCount, const QString &buttonText = {}) {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(10, 8, 10, 6);
        layout->setSpacing(5);
        auto *heading = new QLabel(title);
        heading->setObjectName("pageHeading");
        layout->addWidget(heading);
        layout->addWidget(new QLabel(subtitle));
        layout->addWidget(view, 1);
        auto *footer = new QHBoxLayout;
        if (count) {
            *count = new QLabel(initialCount);
            footer->addWidget(*count);
        }
        footer->addStretch();
        if (!buttonText.isEmpty()) {
            auto *button = new QPushButton(buttonText);
            footer->addWidget(button);
            connect(button, &QPushButton::clicked, this, &TaskManagerWindow::refreshAll);
        }
        layout->addLayout(footer);
        return page;
    }

    void buildPages() {
        tabs = new QTabWidget;
        tabs->setDocumentMode(false);
        tabs->setMovable(false);
        setCentralWidget(tabs);
        buildApplicationsPage();
        buildProcessesPage();
        buildServicesPage();
        buildPerformancePage();
        buildNetworkPage();
        buildUsersPage();
        statusText = new QLabel("Monitoring local system");
        statusBar()->addPermanentWidget(statusText, 1);
        refreshStatus = new QLabel;
        statusBar()->addPermanentWidget(refreshStatus);
        installContextMenus();
    }

    void buildApplicationsPage() {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(10, 8, 10, 6);
        layout->setSpacing(5);
        auto *heading = new QLabel("Applications");
        heading->setObjectName("pageHeading");
        layout->addWidget(heading);
        layout->addWidget(new QLabel("Main applications opened by the current user"));
        applicationList = new QListWidget;
        applicationList->setSelectionMode(QAbstractItemView::SingleSelection);
        applicationList->setSpacing(1);
        applicationList->setUniformItemSizes(true);
        applicationList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        applicationList->setMinimumSize(0, 0);
        layout->addWidget(applicationList, 1);
        auto *footer = new QHBoxLayout;
        applicationCount = new QLabel("0 applications");
        footer->addWidget(applicationCount);
        footer->addStretch();
        auto *end = new QPushButton("End Task");
        auto *switchTo = new QPushButton("Switch To");
        auto *newTask = new QPushButton("New Task...");
        footer->addWidget(end);
        footer->addWidget(switchTo);
        footer->addWidget(newTask);
        connect(end, &QPushButton::clicked, this, &TaskManagerWindow::endApplicationTask);
        connect(switchTo, &QPushButton::clicked, this, &TaskManagerWindow::switchToApplication);
        connect(newTask, &QPushButton::clicked, this, [this] {
            bool accepted = false;
            const QString command = QInputDialog::getText(this, "New Task", "Open:", QLineEdit::Normal, {}, &accepted);
            if (accepted && !command.trimmed().isEmpty()) QProcess::startDetached("sh", {"-c", command});
        });
        layout->addLayout(footer);
        tabs->addTab(page, "Applications");
    }

    void buildProcessesPage() {
        processModel = new QStandardItemModel(this);
        processModel->setHorizontalHeaderLabels({"Image", "PID", "Status", "User", "CPU", "Memory", "Threads", "Description"});
        processView = new QTreeView;
        processModel->setSortRole(SortRole);
        processProxy = new QSortFilterProxyModel(this);
        processProxy->setSourceModel(processModel);
        processProxy->setSortRole(SortRole);
        processProxy->setFilterKeyColumn(-1);
        processProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
        processProxy->setRecursiveFilteringEnabled(true);
        processView->setModel(processProxy);
        processView->setEditTriggers(QAbstractItemView::NoEditTriggers);
        processView->setSelectionBehavior(QAbstractItemView::SelectRows);
        processView->setSelectionMode(QAbstractItemView::ExtendedSelection);
        processView->setUniformRowHeights(true);
        processView->setAnimated(false);
        processView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        processView->setSortingEnabled(true);
        processView->sortByColumn(0, Qt::AscendingOrder);
        processView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        processView->header()->setStretchLastSection(true);
        processView->header()->setMinimumSectionSize(0);
        connect(processView, &QTreeView::collapsed, this, [this](const QModelIndex &index) {
            if (!restoringProcessTree && processSearch && processSearch->text().isEmpty()) {
                collapsedProcesses.insert(index.data(RawPidRole).toInt());
                saveSettings();
            }
        });
        connect(processView, &QTreeView::expanded, this, [this](const QModelIndex &index) {
            if (!restoringProcessTree && processSearch && processSearch->text().isEmpty()) {
                collapsedProcesses.erase(index.data(RawPidRole).toInt());
                saveSettings();
            }
        });
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(10, 8, 10, 6);
        auto *heading = new QLabel("Processes");
        heading->setObjectName("pageHeading");
        layout->addWidget(heading);
        auto *controls = new QHBoxLayout;
        processSearch = new QLineEdit;
        processSearch->setObjectName("processSearch");
        processSearch->setPlaceholderText("Search name, PID, user or command…   Ctrl+F");
        processSearch->setAccessibleName("Search processes");
        processSearch->setClearButtonEnabled(true);
        controls->addWidget(processSearch, 1);
        auto *tree = new QCheckBox("Process tree");
        tree->setChecked(processTree);
        tree->setToolTip("Uncheck for a flat list, then click CPU or Memory to find heavy processes.");
        controls->addWidget(tree);
        layout->addLayout(controls);
        connect(processSearch, &QLineEdit::textChanged, this, [this](const QString &text) {
            processProxy->setFilterFixedString(text);
            restoringProcessTree = true;
            if (!text.isEmpty()) processView->expandAll();
            else restoreProcessExpansion();
            restoringProcessTree = false;
            updateProcessCount();
        });
        connect(tree, &QCheckBox::toggled, this, [this](bool checked) {
            rememberProcessExpansion();
            processTree = checked;
            refreshProcesses(0);
            saveSettings();
        });
        auto *searchAction = new QAction(this);
        searchAction->setShortcut(QKeySequence::Find);
        addAction(searchAction);
        connect(searchAction, &QAction::triggered, this, [this] {
            tabs->setCurrentIndex(1);
            processSearch->setFocus();
            processSearch->selectAll();
        });
        layout->addWidget(processView, 1);
        auto *footer = new QHBoxLayout;
        processCount = new QLabel("0 processes");
        footer->addWidget(processCount);
        footer->addStretch();
        auto *end = new QPushButton("End Selected Processes");
        footer->addWidget(end);
        connect(end, &QPushButton::clicked, this, &TaskManagerWindow::endSelectedProcess);
        layout->addLayout(footer);
        tabs->addTab(page, "Processes");
    }

    QString selectedService() const {
        return serviceList && serviceList->currentItem()
            ? serviceList->currentItem()->data(Qt::UserRole).toString() : QString();
    }

    void updateServiceActions() {
        const QString unit = selectedService();
        for (const auto &[verb, action] : serviceActions) {
            action->setEnabled(!serviceConfirming && !serviceController.busy() &&
                serviceController.available() && ServiceController::validRequest(verb, unit));
        }
    }

    void requestServiceAction(const QString &verb) {
        const QString unit = selectedService(); // Snapshot before any modal dialog/refresh.
        if (serviceController.busy() || serviceConfirming || !ServiceController::validRequest(verb, unit)) return;
        if (verb == "stop" || verb == "restart") {
            serviceConfirming = true;
            updateServiceActions();
            const auto answer = QMessageBox::question(this, "Confirm service operation",
                QString("%1 %2?\n\nThis may interrupt applications or connections that depend on this service.")
                    .arg(verb == "stop" ? "Stop" : "Restart", unit),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
            serviceConfirming = false;
            updateServiceActions();
            if (answer != QMessageBox::Yes) return;
        }
        pendingService = unit;
        pendingServiceVerb = verb;
        ++serviceGeneration;
        if (!serviceController.run(verb, unit)) {
            serviceOperationStatus->setText(serviceController.errorText());
        } else if (serviceController.busy()) {
            serviceOperationStatus->setText(QString("%1: %2 — waiting for authorization or service completion…").arg(unit, verb));
        }
        updateServiceActions();
    }

    void buildServicesPage() {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(10, 8, 10, 6);
        layout->setSpacing(5);
        auto *heading = new QLabel("Services");
        heading->setObjectName("pageHeading");
        layout->addWidget(heading);
        layout->addWidget(new QLabel("Service units managed by the local service manager"));
        serviceList = new QListWidget;
        serviceList->setSelectionMode(QAbstractItemView::SingleSelection);
        serviceList->setSpacing(1);
        serviceList->setUniformItemSizes(true);
        serviceList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        serviceList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        serviceList->setMinimumSize(0, 0);
        layout->addWidget(serviceList, 1);
        connect(serviceList, &QListWidget::currentItemChanged, this, [this] { updateServiceActions(); });
        serviceOperationStatus = new QLabel(serviceController.available()
            ? "Select a service. Administrator authorization is handled by your desktop."
            : "Service control needs systemctl, pkexec, and a desktop polkit authentication agent.");
        serviceOperationStatus->setObjectName("serviceOperationStatus");
        serviceOperationStatus->setTextFormat(Qt::PlainText);
        serviceOperationStatus->setWordWrap(true);
        layout->addWidget(serviceOperationStatus);
        serviceController.finished = [this](ServiceOutcome outcome, const QString &detail) {
            ++serviceGeneration;
            const QString target = QString("%1: %2").arg(pendingService, pendingServiceVerb);
            serviceOperationStatus->setText(outcome == ServiceOutcome::Success
                ? target + " completed." : target + " — " + detail.section('\n', 0, 0));
            serviceOperationStatus->setToolTip(detail);
            updateServiceActions();
            refreshServices();
            if (outcome == ServiceOutcome::Failed) {
                auto *message = new QMessageBox(QMessageBox::Warning, "Service operation failed", {}, QMessageBox::Ok, this);
                message->setTextFormat(Qt::PlainText);
                message->setText(target + "\n\n" + detail);
                message->setAttribute(Qt::WA_DeleteOnClose);
                message->open();
            }
        };
        auto *footer = new QHBoxLayout;
        serviceCount = new QLabel("0 services");
        footer->addWidget(serviceCount);
        footer->addStretch();
        for (const auto &[verb, label] : QList<QPair<QString, QString>>{
                {"start", "Start"}, {"stop", "Stop"}, {"restart", "Restart"}, {"reload", "Reload"}}) {
            auto *action = new QAction(label, this);
            action->setObjectName("serviceAction_" + verb);
            action->setToolTip(verb == "reload" ? "Reload the service's configuration, if supported" : label + " the selected system service");
            serviceActions[verb] = action;
            connect(action, &QAction::triggered, this, [this, verb] { requestServiceAction(verb); });
            auto *button = new QPushButton(label);
            button->setToolTip(action->toolTip());
            connect(button, &QPushButton::clicked, action, &QAction::trigger);
            connect(action, &QAction::changed, button, [action, button] { button->setEnabled(action->isEnabled()); });
            footer->addWidget(button);
        }
        updateServiceActions();
        auto *refresh = new QPushButton("Refresh");
        footer->addWidget(refresh);
        connect(refresh, &QPushButton::clicked, this, &TaskManagerWindow::refreshAll);
        layout->addLayout(footer);
        tabs->addTab(page, "Services");
    }

    QWidget *metricPanel(const QString &title, QLabel **value, const std::vector<double> *history) {
        auto *box = group(title);
        auto *layout = new QVBoxLayout(box);
        *value = valueLabel("Waiting for data...");
        layout->addWidget(*value);
        auto *graph = new HistoryWidget(history);
        graph->setMinimumHeight(45);
        layout->addWidget(graph, 1);
        return box;
    }

    void setCpuView(bool perCore) {
        perCoreGraphs = perCore;
        coreScroll->setVisible(perCore);
        totalCpuGraph->setVisible(!perCore);
        cpuViewHint->setText(perCore ? "Click any core graph for total view" : "Click graph for per-core view");
        const QSignalBlocker blocker(perCoreAction);
        perCoreAction->setChecked(perCore);
        saveSettings();
    }

    void cycleCpuView() {
        setCpuView(!perCoreGraphs);
        if (perCoreGraphs && !coreGraphs.empty()) coreGraphs.front()->setFocus();
        else totalCpuGraph->setFocus();
    }

    void rebuildCoreGraphs(size_t count) {
        while (auto *item = coreGraphsLayout->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        corePanels.clear();
        coreGraphs.clear();
        // Destroy graph pointers before a vector resize can move their histories.
        coreHistories.resize(count);
        for (size_t core = 0; core < count; ++core) {
            auto *panel = group(QString("CPU %1").arg(core));
            panel->setMinimumHeight(95);
            auto *layout = new QVBoxLayout(panel);
            layout->setContentsMargins(4, 8, 4, 4);
            auto *graph = new HistoryWidget(&coreHistories[core]);
            graph->makeInteractive([this] { cycleCpuView(); }, "Show total CPU usage (click, Space or Enter)");
            layout->addWidget(graph, 1);
            coreGraphsLayout->addWidget(panel, static_cast<int>(core) / 4, static_cast<int>(core) % 4);
            corePanels.push_back(panel);
            coreGraphs.push_back(graph);
        }
        if (coreScroll) coreScroll->setMinimumHeight(std::clamp(static_cast<int>((count + 3) / 4) * 100 + 4, 140, 304));
    }

    void buildPerformancePage() {
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto *page = new QWidget;
        auto *layout = new QGridLayout(page);
        layout->setContentsMargins(10, 8, 10, 8);
        layout->setSpacing(7);
        // Keep all three history sections visible. Without explicit row
        // stretch, the CPU grid consumes the available height and the
        // memory/swap histories collapse to their label height.
        layout->setRowStretch(0, 4);
        layout->setRowStretch(1, 2);
        layout->setRowStretch(2, 2);
        layout->setRowStretch(3, 0);
        auto *cpuPanel = group("CPU Usage History");
        auto *cpuLayout = new QVBoxLayout(cpuPanel);
        performanceCpu = valueLabel("Waiting for data...");
        cpuLayout->addWidget(performanceCpu);
        coreGraphsHost = new QWidget;
        coreGraphsLayout = new QGridLayout(coreGraphsHost);
        coreGraphsLayout->setContentsMargins(0, 0, 0, 0);
        coreGraphsLayout->setSpacing(5);
        const size_t coreCount = std::max<size_t>(1, readCpuCoreTicks().size());
        rebuildCoreGraphs(coreCount);
        coreScroll = new QScrollArea;
        coreScroll->setWidgetResizable(true);
        coreScroll->setFrameShape(QFrame::NoFrame);
        coreScroll->setMinimumHeight(std::clamp(static_cast<int>((coreCount + 3) / 4) * 100 + 4, 140, 304));
        coreScroll->setWidget(coreGraphsHost);
        cpuLayout->addWidget(coreScroll, 1);
        totalCpuGraph = new HistoryWidget(&cpuHistory);
        totalCpuGraph->setMinimumHeight(90);
        totalCpuGraph->makeInteractive([this] { cycleCpuView(); }, "Show CPU usage per logical processor (click, Space or Enter)");
        cpuLayout->addWidget(totalCpuGraph, 1);
        coreScroll->setVisible(perCoreGraphs);
        totalCpuGraph->setVisible(!perCoreGraphs);
        cpuViewHint = new QLabel(perCoreGraphs ? "Click any core graph for total view" : "Click graph for per-core view");
        cpuViewHint->setObjectName("graphHint");
        cpuViewHint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        cpuLayout->addWidget(cpuViewHint);
        auto *cpuRow = new QWidget;
        auto *cpuRowLayout = new QHBoxLayout(cpuRow);
        cpuRowLayout->setContentsMargins(0, 0, 0, 0);
        cpuRowLayout->addWidget(usageBlock("CPU Usage", &overviewCpu, &cpuMeter), 0);
        cpuRowLayout->addWidget(cpuPanel, 1);
        layout->addWidget(cpuRow, 0, 0, 1, 2);

        auto *memoryRow = new QWidget;
        auto *memoryRowLayout = new QHBoxLayout(memoryRow);
        memoryRowLayout->setContentsMargins(0, 0, 0, 0);
        memoryRowLayout->addWidget(usageBlock("Physical Memory", &overviewMemory, &memoryMeter), 0);
        memoryRowLayout->addWidget(metricPanel("Physical Memory Usage History", &performanceMemory, &memoryHistory), 1);
        layout->addWidget(memoryRow, 1, 0, 1, 2);

        auto *swapRow = new QWidget;
        auto *swapRowLayout = new QHBoxLayout(swapRow);
        swapRowLayout->setContentsMargins(0, 0, 0, 0);
        swapRowLayout->addWidget(usageBlock("Swap", &overviewSwap, &swapMeter), 0);
        swapRowLayout->addWidget(metricPanel("Swap Usage History", &performanceSwap, &swapHistory), 1);
        layout->addWidget(swapRow, 2, 0, 1, 2);
        auto *stats = group("Totals");
        auto *statsLayout = new QFormLayout(stats);
        statsLayout->addRow("CPU", performanceCpuTotal = valueLabel("Waiting for data..."));
        statsLayout->addRow("Memory", performanceMemoryTotal = valueLabel("Waiting for data..."));
        statsLayout->addRow("Processes", performanceProcessCount = valueLabel("Waiting for data..."));
        statsLayout->addRow("Threads", performanceThreadCount = valueLabel("Waiting for data..."));
        statsLayout->addRow("Available memory", performanceAvailableMemory = valueLabel("Waiting for data..."));
        statsLayout->addRow("Committed / limit", performanceCommit = valueLabel("Waiting for data..."));
        statsLayout->addRow("Kernel", performanceKernel = valueLabel("Waiting for data..."));
        layout->addWidget(stats, 3, 0, 1, 2);
        scroll->setWidget(page);
        tabs->addTab(scroll, "Performance");
    }

    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::Resize &&
            ((networkScroll && watched == networkScroll->viewport()) ||
             (networkClassicScroll && watched == networkClassicScroll->viewport())))
            layoutNetworkCards();
        return QMainWindow::eventFilter(watched, event);
    }

    void setNetworkLayout(const QString &mode) {
        if (mode != "classic" && mode != "cards") return;
        networkLayout = mode;
        networkLayouts->setCurrentIndex(mode == "classic" ? 0 : 1);
        networkClassicAction->setChecked(mode == "classic");
        networkCardsAction->setChecked(mode == "cards");
        layoutNetworkCards();
        saveSettings();
    }

    void syncNetworkList() {
        const QString selected = networkList->currentIndex().siblingAtColumn(0).data().toString();
        const int scroll = networkList->verticalScrollBar()->value();
        const int sortColumn = networkList->header()->sortIndicatorSection();
        const auto sortOrder = networkList->header()->sortIndicatorOrder();
        networkList->setSortingEnabled(false);
        networkListModel->removeRows(0, networkListModel->rowCount());
        for (const auto &[name, card] : networkCards) {
            if (!showLoopback && name == "lo") continue;
            QList<QStandardItem *> row = {new QStandardItem(qs(name)), new QStandardItem(card->receive->text()),
                new QStandardItem(card->transmit->text()), new QStandardItem(formatBytes(card->receivedBytes)),
                new QStandardItem(formatBytes(card->transmittedBytes)), new QStandardItem(card->state->text())};
            const QList<QVariant> values = {qs(name), QVariant::fromValue<qulonglong>(card->receiveRate),
                QVariant::fromValue<qulonglong>(card->transmitRate), QVariant::fromValue<qulonglong>(card->receivedBytes),
                QVariant::fromValue<qulonglong>(card->transmittedBytes), card->state->text()};
            row[0]->setIcon(style()->standardIcon(QStyle::SP_DriveNetIcon));
            for (int column = 0; column < row.size(); ++column) {
                row[column]->setEditable(false);
                row[column]->setData(values[column], SortRole);
                row[column]->setToolTip(card->copyText);
                if (column > 0 && column < 5) row[column]->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            }
            networkListModel->appendRow(row);
        }
        networkList->setSortingEnabled(true);
        networkList->sortByColumn(sortColumn, sortOrder);
        for (int row = 0; row < networkListModel->rowCount(); ++row)
            if (networkListModel->index(row, 0).data().toString() == selected)
                networkList->setCurrentIndex(networkListModel->index(row, 0));
        networkList->verticalScrollBar()->setValue(scroll);
    }

    void layoutNetworkCards() {
        if (!networkGraphsLayout || !networkScroll) return;
        const int columns = std::max(1, std::min(3, networkScroll->viewport()->width() / 370));
        // Remove only layout items: card widgets and their history remain stable.
        while (auto *item = networkGraphsLayout->takeAt(0)) delete item;
        int index = 0;
        for (const auto &[name, card] : networkCards) {
            const bool visible = showLoopback || name != "lo";
            card->setVisible(visible);
            if (visible) {
                networkGraphsLayout->addWidget(card, index / columns, index % columns);
                ++index;
            }
        }
        for (int column = 0; column < 3; ++column)
            networkGraphsLayout->setColumnStretch(column, column < columns ? 1 : 0);
        networkEmpty->setVisible(index == 0);
        networkEmpty->setText(networkCards.empty() ? "No network adapters available." : "Only loopback is present. Enable Show loopback to view it.");
        networkSummary->setText(QString("%1 adapter%2 · Rates and traffic history").arg(index).arg(index == 1 ? "" : "s"));
        if (networkClassicGraphsLayout && networkClassicScroll) {
            const int graphColumns = std::max(1, std::min(3, networkClassicScroll->viewport()->width() / 290));
            while (auto *item = networkClassicGraphsLayout->takeAt(0)) delete item;
            int graphIndex = 0;
            for (const auto &[name, graph] : networkClassicGraphs) {
                const bool visible = showLoopback || name != "lo";
                graph->setVisible(visible);
                if (visible) {
                    networkClassicGraphsLayout->addWidget(graph, graphIndex / graphColumns, graphIndex % graphColumns);
                    ++graphIndex;
                }
            }
            for (int column = 0; column < 3; ++column)
                networkClassicGraphsLayout->setColumnStretch(column, column < graphColumns ? 1 : 0);
        }
    }

    void buildNetworkPage() {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(10, 8, 10, 6);
        layout->setSpacing(9);
        auto *heading = new QLabel("Networking");
        heading->setObjectName("pageHeading");
        layout->addWidget(heading);
        auto *toolbar = new QHBoxLayout;
        networkSummary = new QLabel("Discovering adapters…");
        toolbar->addWidget(networkSummary, 1);
        auto *loopback = new QCheckBox("Show loopback");
        loopback->setObjectName("showLoopback");
        loopback->setChecked(showLoopback);
        connect(loopback, &QCheckBox::toggled, this, [this](bool visible) {
            showLoopback = visible;
            syncNetworkList();
            layoutNetworkCards();
            saveSettings();
        });
        toolbar->addWidget(loopback);
        layout->addLayout(toolbar);
        networkEmpty = new QLabel;
        networkEmpty->setWordWrap(true);
        layout->addWidget(networkEmpty);
        networkLayouts = new QStackedWidget;
        auto *classic = new QSplitter(Qt::Vertical);
        networkClassicScroll = new QScrollArea;
        networkClassicScroll->setWidgetResizable(true);
        networkClassicScroll->setFrameShape(QFrame::NoFrame);
        networkClassicScroll->setMinimumHeight(160);
        networkClassicScroll->viewport()->installEventFilter(this);
        auto *classicGraphsHost = new QWidget;
        networkClassicGraphsLayout = new QGridLayout(classicGraphsHost);
        networkClassicGraphsLayout->setContentsMargins(0, 0, 2, 0);
        networkClassicGraphsLayout->setSpacing(8);
        networkClassicScroll->setWidget(classicGraphsHost);
        classic->addWidget(networkClassicScroll);
        auto *listPanel = new QWidget;
        auto *listLayout = new QVBoxLayout(listPanel);
        listLayout->setContentsMargins(0, 6, 0, 0);
        listLayout->addWidget(new QLabel("Adapters · Green: receive · Yellow: transmit"));
        networkListModel = new QStandardItemModel(this);
        networkListModel->setHorizontalHeaderLabels({"Adapter", "Receive / s", "Transmit / s", "Received", "Sent", "State"});
        networkListModel->setSortRole(SortRole);
        networkList = new QTreeView;
        networkList->setObjectName("networkList");
        networkList->setModel(networkListModel);
        networkList->setRootIsDecorated(false);
        networkList->setItemsExpandable(false);
        networkList->setUniformRowHeights(true);
        networkList->setAllColumnsShowFocus(true);
        networkList->setEditTriggers(QAbstractItemView::NoEditTriggers);
        networkList->setSelectionBehavior(QAbstractItemView::SelectRows);
        networkList->setSelectionMode(QAbstractItemView::SingleSelection);
        networkList->setSortingEnabled(true);
        networkList->sortByColumn(0, Qt::AscendingOrder);
        networkList->setFrameShape(QFrame::NoFrame);
        networkList->header()->setStretchLastSection(true);
        for (int column = 0; column < 6; ++column) networkList->setColumnWidth(column, column == 0 ? 155 : 115);
        listLayout->addWidget(networkList);
        classic->addWidget(listPanel);
        classic->setCollapsible(0, false);
        classic->setCollapsible(1, false);
        classic->setStretchFactor(0, 2);
        classic->setStretchFactor(1, 1);
        classic->setSizes({320, 180});
        networkLayouts->addWidget(classic);
        networkScroll = new QScrollArea;
        networkScroll->setWidgetResizable(true);
        networkScroll->setFrameShape(QFrame::NoFrame);
        networkScroll->viewport()->installEventFilter(this);
        networkGraphsHost = new QWidget;
        networkGraphsLayout = new QGridLayout(networkGraphsHost);
        networkGraphsLayout->setContentsMargins(0, 0, 2, 0);
        networkGraphsLayout->setSpacing(12);
        networkGraphsLayout->setAlignment(Qt::AlignTop);
        networkScroll->setWidget(networkGraphsHost);
        networkLayouts->addWidget(networkScroll);
        networkLayouts->setCurrentIndex(networkLayout == "classic" ? 0 : 1);
        layout->addWidget(networkLayouts, 1);
        auto *footer = new QHBoxLayout;
        footer->addWidget(new QLabel("Each graph scales independently · Totals since adapter reset"));
        footer->addStretch();
        auto *refresh = new QPushButton("Refresh");
        footer->addWidget(refresh);
        connect(refresh, &QPushButton::clicked, this, &TaskManagerWindow::refreshAll);
        layout->addLayout(footer);
        tabs->addTab(page, "Networking");
    }

    void buildUsersPage() {
        userModel = new QStandardItemModel(this);
        userModel->setHorizontalHeaderLabels({"User / Process", "Status", "Processes", "CPU", "Memory", "PID"});
        userView = new QTreeView;
        userView->setModel(userModel);
        userView->setSelectionBehavior(QAbstractItemView::SelectRows);
        userView->setSelectionMode(QAbstractItemView::ExtendedSelection);
        userView->setEditTriggers(QAbstractItemView::NoEditTriggers);
        userView->setUniformRowHeights(true);
        userView->setAnimated(false);
        userView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        userView->header()->setStretchLastSection(true);
        userView->header()->setMinimumSectionSize(0);
        connect(userView, &QTreeView::collapsed, this, [this](const QModelIndex &index) {
            if (!restoringUserTree && !index.parent().isValid()) {
                collapsedUsers.insert(index.data().toString());
                saveSettings();
            }
        });
        connect(userView, &QTreeView::expanded, this, [this](const QModelIndex &index) {
            if (!restoringUserTree && !index.parent().isValid()) {
                collapsedUsers.erase(index.data().toString());
                saveSettings();
            }
        });
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(10, 8, 10, 6);
        auto *heading = new QLabel("Users");
        heading->setObjectName("pageHeading");
        layout->addWidget(heading);
        layout->addWidget(new QLabel("Process owners; process children are optional from View"));
        layout->addWidget(userView, 1);
        userCount = new QLabel("0 users, 0 processes");
        layout->addWidget(userCount);
        tabs->addTab(page, "Users");
    }

    QWidget *usageBlock(const QString &title, QLabel **label, MeterWidget **meter) {
        auto *box = group(title);
        box->setMinimumWidth(box->fontMetrics().horizontalAdvance(title) + 28);
        auto *layout = new QVBoxLayout(box);
        *meter = new MeterWidget;
        (*meter)->setFixedWidth(48);
        layout->addWidget(*meter, 1, Qt::AlignHCenter);
        *label = valueLabel("0.0%");
        (*label)->setAlignment(Qt::AlignCenter);
        layout->addWidget(*label);
        return box;
    }

    QWidget *historyBlock(const QString &title, const std::vector<double> *history) {
        auto *box = group(title);
        auto *layout = new QVBoxLayout(box);
        layout->addWidget(new HistoryWidget(history), 1);
        return box;
    }

    static std::vector<int> sortedRoots(const std::map<int, const ProcessInfo *> &byPid,
                                        const std::map<int, std::vector<int>> &children) {
        std::vector<int> roots;
        for (const auto &[pid, process] : byPid)
            if (process->ppid <= 0 || byPid.find(process->ppid) == byPid.end()) roots.push_back(pid);
        std::sort(roots.begin(), roots.end(), [&](int left, int right) {
            return byPid.at(left)->name == byPid.at(right)->name ? left < right
                                                                   : byPid.at(left)->name < byPid.at(right)->name;
        });
        (void)children;
        return roots;
    }

    QList<QStandardItem *> processRow(const ProcessInfo &process) const {
        QList<QStandardItem *> row;
        row << new QStandardItem(qs(process.name).simplified())
            << new QStandardItem(QString::number(process.pid))
            << new QStandardItem(qs(process.status))
            << new QStandardItem(qs(process.user))
            << new QStandardItem(percentText(process.cpu))
            << new QStandardItem(formatBytes(static_cast<unsigned long long>(process.residentPages) * getpagesize()))
            << new QStandardItem(QString::number(process.threads))
            << new QStandardItem(qs(process.command).simplified());
        const QList<QVariant> sortValues = {qs(process.name), process.pid, qs(process.status), qs(process.user),
            process.cpu, QVariant::fromValue<qulonglong>(static_cast<unsigned long long>(process.residentPages) * getpagesize()),
            process.threads, qs(process.command)};
        for (int column = 0; column < row.size(); ++column) {
            row[column]->setData(process.pid, RawPidRole);
            row[column]->setData(sortValues[column], SortRole);
            row[column]->setToolTip(qs(process.command));
            row[column]->setEditable(false);
        }
        row[0]->setData(process.ppid, ParentPidRole);
        return row;
    }

    void updateProcessRow(QStandardItem *item, const ProcessInfo &process) {
        QStandardItem *owner = item->parent() ? item->parent() : processModel->invisibleRootItem();
        const int rowIndex = item->row();
        const auto fresh = processRow(process);
        for (int column = 0; column < fresh.size(); ++column) {
            QStandardItem *cell = owner->child(rowIndex, column);
            cell->setText(fresh[column]->text());
            cell->setData(fresh[column]->data(SortRole), SortRole);
            cell->setToolTip(fresh[column]->toolTip());
            delete fresh[column];
        }
        item->setData(process.ppid, ParentPidRole);
    }

    void appendProcessBranch(QStandardItem *parent, int pid, const std::map<int, const ProcessInfo *> &byPid,
                             const std::map<int, std::vector<int>> &children, std::vector<std::pair<int, int>> &structure) {
        const ProcessInfo *process = byPid.at(pid);
        structure.emplace_back(pid, parent ? parent->data(RawPidRole).toInt() : 0);
        const QList<QStandardItem *> row = processRow(*process);
        QStandardItem *actual = row.first();
        if (parent) parent->appendRow(row);
        else processModel->invisibleRootItem()->appendRow(row);
        const auto found = children.find(pid);
        if (found != children.end()) {
            for (int child : found->second) appendProcessBranch(actual, child, byPid, children, structure);
        }
    }

    static void collectDesiredBranch(int pid, int parentPid, const std::map<int, std::vector<int>> &children,
                                     std::vector<std::pair<int, int>> &structure) {
        structure.emplace_back(pid, parentPid);
        const auto found = children.find(pid);
        if (found != children.end()) {
            for (const int child : found->second) collectDesiredBranch(child, pid, children, structure);
        }
    }

    static void collectStructure(QStandardItem *parent, std::vector<std::pair<int, int>> &result, int parentPid = 0) {
        for (int row = 0; row < parent->rowCount(); ++row) {
            QStandardItem *item = parent->child(row);
            const int pid = item->data(RawPidRole).toInt();
            result.emplace_back(pid, parentPid);
            collectStructure(item, result, pid);
        }
    }

    QString processAnchor() const {
        const QModelIndex index = processView->indexAt(QPoint(2, 2));
        return index.isValid() ? QString::number(index.data(RawPidRole).toInt()) : QString();
    }

    QModelIndex findPid(QAbstractItemModel *model, int pid, const QModelIndex &parent = {}) const {
        for (int row = 0; row < model->rowCount(parent); ++row) {
            const QModelIndex index = model->index(row, 0, parent);
            if (index.data(RawPidRole).toInt() == pid) return index;
            const QModelIndex child = findPid(model, pid, index);
            if (child.isValid()) return child;
        }
        return {};
    }

    struct UserKey {
        QString name;
        QString id;
        bool operator==(const UserKey &other) const { return name == other.name && id == other.id; }
    };

    static void collectUserStructure(QStandardItem *parent, std::vector<UserKey> &result) {
        for (int row = 0; row < parent->rowCount(); ++row) {
            QStandardItem *item = parent->child(row);
            result.push_back({item->text(), item->data(RawPidRole).toString()});
            collectUserStructure(item, result);
        }
    }

    QModelIndex findUserKey(const QString &name, const QString &id, const QModelIndex &parent = {}) const {
        for (int row = 0; row < userModel->rowCount(parent); ++row) {
            const QModelIndex index = userModel->index(row, 0, parent);
            if (index.data().toString() == name && index.data(RawPidRole).toString() == id) return index;
            const QModelIndex child = findUserKey(name, id, index);
            if (child.isValid()) return child;
        }
        return {};
    }

    void rememberUserExpansion() {
        // QTreeView emits expansion signals for some model changes, but not
        // every refresh path. Snapshot the actual view state before touching
        // the model so a rebuild cannot reopen user rows the operator closed.
        if (!showUserProcesses || !userModel || userModel->rowCount() == 0) return;
        std::set<QString> currentCollapsed;
        for (int row = 0; row < userModel->rowCount(); ++row) {
            const QModelIndex index = userModel->index(row, 0);
            if (userModel->hasChildren(index) && !userView->isExpanded(index))
                currentCollapsed.insert(index.data(RawPidRole).toString());
        }
        collapsedUsers = std::move(currentCollapsed);
    }

    void restoreProcessExpansion() {
        restoringProcessTree = true;
        processView->expandAll();
        std::function<void(const QModelIndex &)> collapse = [&](const QModelIndex &parent) {
            for (int row = 0; row < processModel->rowCount(parent); ++row) {
                const QModelIndex index = processModel->index(row, 0, parent);
                if (collapsedProcesses.count(index.data(RawPidRole).toInt())) processView->collapse(processProxy->mapFromSource(index));
                collapse(index);
            }
        };
        collapse({});
        restoringProcessTree = false;
    }

    void updateProcessCount() {
        int matches = 0;
        const QString query = processSearch ? processSearch->text() : QString();
        for (const auto &process : processes) {
            const QString searchable = QString("%1 %2 %3 %4 %5 %6 %7 %8")
                .arg(qs(process.name)).arg(process.pid).arg(qs(process.status), qs(process.user),
                    percentText(process.cpu), formatBytes(static_cast<unsigned long long>(process.residentPages) * getpagesize()))
                .arg(process.threads).arg(qs(process.command));
            if (searchable.contains(query, Qt::CaseInsensitive)) ++matches;
        }
        processCount->setText(query.isEmpty() ? QString("%1 processes").arg(processes.size())
            : QString("%1 matches / %2 processes (ancestors retained)").arg(matches).arg(processes.size()));
    }

    void refreshProcesses(double cpu) {
        const auto viewState = rememberView(processView);
        const QByteArray header = processView->header()->saveState();
        processProxy->setDynamicSortFilter(false);
        if (processTree == renderedProcessTree) rememberProcessExpansion();
        std::map<int, const ProcessInfo *> byPid;
        std::map<int, std::vector<int>> children;
        for (const ProcessInfo &process : processes) {
            byPid[process.pid] = &process;
            if (processTree) children[process.ppid].push_back(process.pid);
        }
        for (auto &[parent, list] : children) {
            std::sort(list.begin(), list.end(), [&](int left, int right) {
                return byPid.at(left)->name == byPid.at(right)->name ? left < right
                                                                       : byPid.at(left)->name < byPid.at(right)->name;
            });
        }
        std::vector<std::pair<int, int>> desired;
        std::vector<int> roots = sortedRoots(byPid, children);
        if (!processTree) {
            roots.clear();
            for (const auto &[pid, process] : byPid) roots.push_back(pid);
        }
        for (int root : roots) collectDesiredBranch(root, 0, children, desired);
        std::vector<std::pair<int, int>> current;
        collectStructure(processModel->invisibleRootItem(), current);
        if (current == desired) {
            std::function<void(QStandardItem *)> update = [&](QStandardItem *parent) {
                for (int row = 0; row < parent->rowCount(); ++row) {
                    QStandardItem *item = parent->child(row);
                    const auto found = byPid.find(item->data(RawPidRole).toInt());
                    if (found != byPid.end()) updateProcessRow(item, *found->second);
                    update(item);
                }
            };
            update(processModel->invisibleRootItem());
        } else {
            const QString anchor = processAnchor();
            restoringProcessTree = true;
            processModel->removeRows(0, processModel->rowCount());
            desired.clear();
            for (int root : roots) appendProcessBranch(nullptr, root, byPid, children, desired);
            restoreProcessExpansion();
            if (!anchor.isEmpty()) {
                const QModelIndex index = findPid(processProxy, anchor.toInt());
                if (index.isValid()) processView->scrollTo(index, QAbstractItemView::PositionAtTop);
            }
        }
        processProxy->setDynamicSortFilter(true);
        processProxy->invalidate();
        processView->header()->restoreState(header);
        if (processSearch && !processSearch->text().isEmpty()) {
            restoringProcessTree = true;
            processView->expandAll();
            restoringProcessTree = false;
        }
        restoreView(processView, viewState);
        renderedProcessTree = processTree;
        updateProcessCount();
        (void)cpu;
    }

    // Helpers run asynchronously; a missing or stalled desktop/service tool never
    // makes the GUI wait. A timeout also bounds the lifetime of each child.
    void runDiscovery(const QString &program, const QStringList &arguments,
                      std::function<void(bool, const QString &, const QString &)> completed) {
        auto *command = new QProcess(this);
        auto *deadline = new QTimer(command);
        deadline->setSingleShot(true);
        auto done = std::make_shared<bool>(false);
        auto finish = [command, deadline, completed, done](bool ok, const QString &reason) {
            if (*done) return;
            *done = true;
            deadline->stop();
            const QString output = QString::fromLocal8Bit(command->readAllStandardOutput());
            const QString errors = QString::fromLocal8Bit(command->readAllStandardError()).trimmed();
            completed(ok, output, reason.isEmpty() ? errors : reason);
            command->deleteLater();
        };
        connect(command, &QProcess::finished, this, [finish](int code, QProcess::ExitStatus status) {
            finish(status == QProcess::NormalExit && code == 0,
                   status == QProcess::CrashExit ? "Command stopped or timed out" : QString());
        });
        connect(command, &QProcess::errorOccurred, this, [finish, command](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) finish(false, command->errorString());
        });
        connect(deadline, &QTimer::timeout, command, &QProcess::kill);
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert("LC_ALL", "C");
        command->setProcessEnvironment(environment);
        command->start(program, arguments);
        deadline->start(2000);
    }

    void refreshApplications() {
        if (windowsPending) return;
        windowsPending = true;
        runDiscovery("wmctrl", {"-l", "-p"}, [this](bool ok, const QString &output, const QString &) {
            windowsPending = false;
            const auto *selected = applicationList->currentItem();
            const int selectedPid = selected ? selected->data(Qt::UserRole).toInt() : 0;
            const QString selectedWindow = selected ? selected->data(WindowIdRole).toString() : QString();
            const int scroll = applicationList->verticalScrollBar()->value();
            applicationList->clear();
            const QString current = qs(currentUserName());
            std::map<int, const ProcessInfo *> byPid;
            for (const ProcessInfo &process : processes) byPid[process.pid] = &process;
            if (ok) {
                for (const QString &line : output.split('\n', Qt::SkipEmptyParts)) {
                    const QStringList fields = line.simplified().split(' ');
                    if (fields.size() < 5) continue;
                    bool parsed = false;
                    const int pid = fields[2].toInt(&parsed);
                    const auto process = byPid.find(pid);
                    if (!parsed || process == byPid.end() || qs(process->second->user) != current) continue;
                    const QString title = fields.mid(4).join(' ');
                    auto *item = new QListWidgetItem(style()->standardIcon(QStyle::SP_DesktopIcon),
                        title.isEmpty() ? qs(process->second->name) : title);
                    item->setData(Qt::UserRole, pid);
                    item->setData(WindowIdRole, fields[0]);
                    item->setToolTip(QString("%1 (PID %2)").arg(qs(process->second->name)).arg(pid));
                    applicationList->addItem(item);
                }
            } else {
                for (const ProcessInfo &process : processes) {
                    if (qs(process.user) != current || process.command.find('/') == std::string::npos) continue;
                    auto *item = new QListWidgetItem(style()->standardIcon(QStyle::SP_DesktopIcon), qs(process.name));
                    item->setData(Qt::UserRole, process.pid);
                    item->setToolTip(QString("Session process (PID %1) — window information unavailable").arg(process.pid));
                    applicationList->addItem(item);
                }
            }
            for (int row = 0; row < applicationList->count(); ++row) {
                auto *item = applicationList->item(row);
                if (item->data(Qt::UserRole).toInt() == selectedPid &&
                    item->data(WindowIdRole).toString() == selectedWindow) applicationList->setCurrentItem(item);
            }
            applicationList->verticalScrollBar()->setValue(scroll);
            applicationCount->setText(QString::number(applicationList->count()) +
                (ok ? " X11 windows" : " session processes · window list unavailable"));
        });
    }

    void applyServiceSnapshot(bool ok, const QString &output, const QString &error) {
        if (!ok) {
            serviceList->clear();
            serviceCount->setText("Service query failed");
            serviceList->addItem("Unable to query systemd services");
            serviceList->item(0)->setToolTip(error);
            updateServiceActions();
            return;
        }
        const QString selected = selectedService();
        const auto *topItem = serviceList->itemAt(QPoint(2, 2));
        const QString topUnit = topItem ? topItem->data(Qt::UserRole).toString() : QString();
        const int topOffset = topItem ? serviceList->visualItemRect(topItem).top() : 0;
        const int vertical = serviceList->verticalScrollBar()->value();
        const int horizontal = serviceList->horizontalScrollBar()->value();
        const QSignalBlocker signalBlocker(serviceList);
        serviceList->setUpdatesEnabled(false);
        std::map<QString, QListWidgetItem *> existing;
        for (int row = 0; row < serviceList->count(); ++row) {
            auto *item = serviceList->item(row);
            existing[item->data(Qt::UserRole).toString()] = item;
        }
        std::set<QString> seen;
        int row = 0;
        for (const QString &line : output.split('\n', Qt::SkipEmptyParts)) {
            const QStringList fields = line.simplified().split(' ');
            if (fields.size() < 4 || !seen.insert(fields[0]).second) continue;
            const auto found = existing.find(fields[0]);
            auto *item = found != existing.end() ? found->second : new QListWidgetItem;
            if (found == existing.end()) {
                serviceList->insertItem(row, item);
            } else if (serviceList->row(item) != row) {
                serviceList->takeItem(serviceList->row(item));
                serviceList->insertItem(row, item);
            }
            item->setText(QString("%1    %2 / %3    %4").arg(fields[0], fields[2], fields[3], fields.mid(4).join(' ')));
            item->setData(Qt::UserRole, fields[0]);
            item->setToolTip(QString("Load: %1\nActive: %2\nState: %3\n%4").arg(fields[1], fields[2], fields[3], fields.mid(4).join(' ')));
            ++row;
        }
        while (serviceList->count() > row) delete serviceList->takeItem(serviceList->count() - 1);
        QListWidgetItem *selection = nullptr;
        QListWidgetItem *anchor = nullptr;
        for (int index = 0; index < serviceList->count(); ++index) {
            auto *item = serviceList->item(index);
            const QString unit = item->data(Qt::UserRole).toString();
            if (!selected.isEmpty() && unit == selected) selection = item;
            if (!topUnit.isEmpty() && unit == topUnit) anchor = item;
        }
        // Ordinary refreshes leave the existing item and selection intact. After
        // insertions/removals, finish layout before restoring the visible anchor;
        // otherwise a temporarily zero scrollbar range clamps the position to top.
        if (serviceList->currentItem() != selection) serviceList->setCurrentItem(selection);
        serviceList->doItemsLayout();
        serviceList->verticalScrollBar()->setValue(anchor
            ? serviceList->verticalScrollBar()->value() + serviceList->visualItemRect(anchor).top() - topOffset
            : vertical);
        serviceList->horizontalScrollBar()->setValue(horizontal);
        serviceList->setUpdatesEnabled(true);
        serviceCount->setText(QString::number(serviceList->count()) + " services");
        updateServiceActions();
    }

    void refreshServices() {
        if (servicesPending || serviceController.busy()) return;
        const auto generation = serviceGeneration;
        servicesPending = true;
        runDiscovery("systemctl", {"list-units", "--type=service", "--all", "--no-legend", "--no-pager", "--plain"},
            [this, generation](bool ok, const QString &output, const QString &error) {
            servicesPending = false;
            if (generation != serviceGeneration) {
                refreshServices();
                return;
            }
            applyServiceSnapshot(ok, output, error);
        });
    }

    using NetworkCounters = std::map<std::string, std::pair<unsigned long long, unsigned long long>>;

    void updateNetworkCards(const NetworkCounters &interfaces) {
        for (auto it = networkCards.begin(); it != networkCards.end();) {
            if (!interfaces.count(it->first)) {
                delete it->second;
                if (auto graph = networkClassicGraphs.find(it->first); graph != networkClassicGraphs.end()) {
                    delete graph->second;
                    networkClassicGraphs.erase(graph);
                }
                networkReceiveHistories.erase(it->first);
                networkTransmitHistories.erase(it->first);
                if (selectedAdapter == qs(it->first)) selectedAdapter.clear();
                it = networkCards.erase(it);
            } else ++it;
        }
        for (const auto &[name, counters] : interfaces) {
            const auto previous = previousNetworkInterfaces.find(name);
            const auto rxRate = previous != previousNetworkInterfaces.end()
                ? counterRate(counters.first, previous->second.first, sampleSeconds) : 0;
            const auto txRate = previous != previousNetworkInterfaces.end()
                ? counterRate(counters.second, previous->second.second, sampleSeconds) : 0;
            appendHistory(networkReceiveHistories[name], static_cast<double>(rxRate));
            appendHistory(networkTransmitHistories[name], static_cast<double>(txRate));
            if (!networkCards.count(name)) {
                auto *card = new NetworkCard(qs(name), &networkReceiveHistories[name], &networkTransmitHistories[name]);
                card->setParent(networkGraphsHost);
                networkCards[name] = card;
                auto *graphBox = group(qs(name));
                graphBox->setParent(networkClassicScroll->widget());
                graphBox->setMinimumHeight(145);
                auto *graphLayout = new QVBoxLayout(graphBox);
                graphLayout->setContentsMargins(6, 10, 6, 6);
                graphLayout->addWidget(new HistoryWidget(&networkReceiveHistories[name], &networkTransmitHistories[name], graphBox, true), 1);
                networkClassicGraphs[name] = graphBox;
                installContextMenu(graphBox, 4);
                for (auto *child : graphBox->findChildren<QWidget *>()) installContextMenu(child, 4);
                installContextMenu(card, 4);
                for (auto *child : card->findChildren<QWidget *>()) installContextMenu(child, 4);
            }
            networkCards[name]->setCounters(qs(name), qs(trim(readFile("/sys/class/net/" + name + "/operstate"))),
                counters.first, counters.second, rxRate, txRate);
        }
        previousNetworkInterfaces = interfaces;
        syncNetworkList();
        for (const auto &[name, graph] : networkClassicGraphs) graph->update();
        layoutNetworkCards();
    }

    void refreshNetwork() { updateNetworkCards(readNetworkInterfaces()); }

    void refreshUsers() {
        const auto viewState = rememberView(userView);
        const auto header = userView->header()->saveState();
        rememberUserExpansion();
        std::map<std::string, std::vector<const ProcessInfo *>> byUser;
        for (const ProcessInfo &process : processes) byUser[process.user].push_back(&process);
        const std::string current = currentUserName();
        struct UserTotal { double cpu = 0.0; unsigned long long memory = 0; int count = 0; };
        std::map<QString, UserTotal> totals;
        std::map<int, const ProcessInfo *> byPid;
        std::vector<UserKey> desired;
        int total = 0;
        for (auto &[user, list] : byUser) {
            std::sort(list.begin(), list.end(), [](const ProcessInfo *left, const ProcessInfo *right) {
                return left->name == right->name ? left->pid < right->pid : left->name < right->name;
            });
            double cpu = 0.0;
            unsigned long long memory = 0;
            for (const ProcessInfo *process : list) {
                cpu += process->cpu;
                memory += static_cast<unsigned long long>(process->residentPages) * getpagesize();
                byPid[process->pid] = process;
            }
            totals[qs(user)] = {cpu, memory, static_cast<int>(list.size())};
            desired.push_back({qs(user), qs(user)});
            if (showUserProcesses) {
                for (const ProcessInfo *process : list)
                    desired.push_back({qs(process->name), QString::number(process->pid)});
            }
            total += static_cast<int>(list.size());
        }
        std::vector<UserKey> existing;
        collectUserStructure(userModel->invisibleRootItem(), existing);
        if (existing == desired) {
            std::function<void(QStandardItem *)> update = [&](QStandardItem *parent) {
                for (int row = 0; row < parent->rowCount(); ++row) {
                    QStandardItem *item = parent->child(row);
                    const bool isUser = !item->index().parent().isValid();
                    QStandardItem *rowOwner = item->parent() ? item->parent() : userModel->invisibleRootItem();
                    if (isUser) {
                        const auto found = totals.find(item->data(RawPidRole).toString());
                        if (found != totals.end()) {
                            item->setText(item->data(RawPidRole).toString());
                            rowOwner->child(item->row(), 1)->setText(found->first == qs(current) ? "Current" : "Process owner");
                            rowOwner->child(item->row(), 2)->setText(QString::number(found->second.count));
                            rowOwner->child(item->row(), 3)->setText(percentText(found->second.cpu));
                            rowOwner->child(item->row(), 4)->setText(formatBytes(found->second.memory));
                            rowOwner->child(item->row(), 5)->setText("—");
                        }
                    } else {
                        const auto found = byPid.find(item->data(RawPidRole).toInt());
                        if (found != byPid.end()) {
                            const ProcessInfo &process = *found->second;
                            item->setText(qs(process.name));
                            rowOwner->child(item->row(), 1)->setText(qs(process.status));
                            rowOwner->child(item->row(), 3)->setText(percentText(process.cpu));
                            rowOwner->child(item->row(), 4)->setText(formatBytes(static_cast<unsigned long long>(process.residentPages) * getpagesize()));
                        }
                    }
                    update(item);
                }
            };
            update(userModel->invisibleRootItem());
        } else {
            restoringUserTree = true;
            userModel->removeRows(0, userModel->rowCount());
            userModel->setHorizontalHeaderLabels({"User / Process", "Status", "Processes", "CPU", "Memory", "PID"});
            for (auto &[user, list] : byUser) {
                std::sort(list.begin(), list.end(), [](const ProcessInfo *left, const ProcessInfo *right) {
                    return left->name == right->name ? left->pid < right->pid : left->name < right->name;
                });
                double cpu = 0.0;
                unsigned long long memory = 0;
                for (const ProcessInfo *process : list) {
                    cpu += process->cpu;
                    memory += static_cast<unsigned long long>(process->residentPages) * getpagesize();
                }
            auto *root = new QStandardItem(qs(user));
            root->setData(qs(user), RawPidRole);
            QList<QStandardItem *> rootRow;
            rootRow << root << new QStandardItem(user == current ? "Current" : "Process owner")
                    << new QStandardItem(QString::number(list.size())) << new QStandardItem(percentText(cpu))
                    << new QStandardItem(formatBytes(memory)) << new QStandardItem("—");
            userModel->invisibleRootItem()->appendRow(rootRow);
            if (showUserProcesses) {
                for (const ProcessInfo *process : list) {
                    QList<QStandardItem *> row;
                    row << new QStandardItem(qs(process->name)) << new QStandardItem(qs(process->status))
                        << new QStandardItem("1") << new QStandardItem(percentText(process->cpu))
                        << new QStandardItem(formatBytes(static_cast<unsigned long long>(process->residentPages) * getpagesize()))
                        << new QStandardItem(QString::number(process->pid));
                    for (QStandardItem *item : row) item->setData(process->pid, RawPidRole);
                    root->appendRow(row);
                }
            }
            }
            restoringUserTree = true;
            userView->expandAll();
            for (int row = 0; row < userModel->rowCount(); ++row) {
                const QModelIndex index = userModel->index(row, 0);
                if (collapsedUsers.count(index.data(RawPidRole).toString())) userView->collapse(index);
            }
            restoringUserTree = false;
        }
        userView->header()->restoreState(header);
        restoreView(userView, viewState);
        userCount->setText(QString::number(byUser.size()) + " users, " + QString::number(total) + " processes");
    }

    void updateUptimeAndStatus(double cpu, double memory, double paging, unsigned long long diskRate,
                               unsigned long long networkRate) {
        const std::string uptimeText = readFile("/proc/uptime");
        double seconds = 0.0;
        std::istringstream(uptimeText) >> seconds;
        const auto totalSeconds = static_cast<unsigned long long>(seconds);
        const QString uptime = QString("%1:%2:%3").arg(totalSeconds / 3600)
            .arg((totalSeconds / 60) % 60, 2, 10, QLatin1Char('0'))
            .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'));
        if (overviewUptime) overviewUptime->setText(uptime);
        statusText->setText(QString("Processes: %1    CPU Usage: %2    Physical Memory: %3")
            .arg(processes.size()).arg(percentText(cpu)).arg(percentText(memory)));
        if (overviewCpu) overviewCpu->setText(percentText(cpu));
        if (overviewMemory) overviewMemory->setText(percentText(memory));
        if (overviewSwap) overviewSwap->setText(percentText(paging));
        if (overviewProcesses) overviewProcesses->setText(QString::number(processes.size()));
        if (overviewDisk) overviewDisk->setText(formatRate(diskRate));
        if (overviewNetwork) overviewNetwork->setText(formatRate(networkRate));
        performanceCpu->setText("CPU usage: " + percentText(cpu) + " · History: last 120 samples");
        performanceMemory->setText("Memory usage: " + percentText(memory));
        const auto swapTotal = readMemoryValue("SwapTotal:");
        const auto swapFree = readMemoryValue("SwapFree:");
        const auto swapUsed = swapTotal > swapFree ? swapTotal - swapFree : 0;
        performanceSwap->setText("Swap usage: " + percentText(paging) + "  (" +
                                  formatBytes(swapUsed) + " of " + formatBytes(swapTotal) + ")");
        performanceCpuTotal->setText(percentText(cpu));
        performanceMemoryTotal->setText(percentText(memory));
        performanceProcessCount->setText(QString::number(processes.size()));
        int threads = 0;
        for (const ProcessInfo &process : processes) threads += process.threads;
        performanceThreadCount->setText(QString::number(threads));
        performanceAvailableMemory->setText(formatBytes(readMemoryValue("MemAvailable:")));
        performanceCommit->setText(formatBytes(readMemoryValue("Committed_AS:")) + " / " +
                                  formatBytes(readMemoryValue("CommitLimit:")));
        struct utsname system{};
        uname(&system);
        performanceKernel->setText(system.release);
        cpuMeter->setValue(cpu);
        memoryMeter->setValue(memory);
        swapMeter->setValue(paging);
    }

    void refreshAll() {
        sampleSeconds = sampleClock.isValid() ? sampleClock.nsecsElapsed() / 1.0e9 : 0.0;
        sampleClock.start();
        const auto previousTotal = previousTotalTicks;
        processes = collectProcesses();
        const auto currentCoreTicks = readCpuCoreTicks();
        unsigned long long idle = 0;
        const auto currentTotal = readTotalCpuTicks(idle);
        const auto totalDelta = currentTotal > previousTotal ? currentTotal - previousTotal : 0;
        double cpu = 0.0;
        if (previousTotal != 0 && totalDelta > 0) {
            const auto idleDelta = idle >= previousIdleTicks ? idle - previousIdleTicks : 0;
            cpu = std::clamp(100.0 * (1.0 - static_cast<double>(idleDelta) / totalDelta), 0.0, 100.0);
        }
        if (!currentCoreTicks.empty() && coreHistories.size() != currentCoreTicks.size())
            rebuildCoreGraphs(currentCoreTicks.size());
        if (previousCoreTicks.size() == currentCoreTicks.size() &&
            coreHistories.size() == currentCoreTicks.size()) {
            for (size_t core = 0; core < currentCoreTicks.size(); ++core) {
                const auto &now = currentCoreTicks[core];
                const auto &before = previousCoreTicks[core];
                const auto total = now.total > before.total ? now.total - before.total : 0;
                const auto idleDelta = now.idle >= before.idle ? now.idle - before.idle : 0;
                const double usage = total > 0
                    ? std::clamp(100.0 * (1.0 - static_cast<double>(idleDelta) / total), 0.0, 100.0) : 0.0;
                appendHistory(coreHistories[core], usage);
            }
        } else {
            for (auto &history : coreHistories) appendHistory(history, 0.0);
        }
        previousCoreTicks = currentCoreTicks;
        for (size_t core = 0; core < corePanels.size(); ++core) {
            const auto &history = coreHistories[core];
            corePanels[core]->setTitle(QString("CPU %1 · %2").arg(core).arg(history.empty() ? "—" : percentText(history.back())));
            coreGraphs[core]->update();
        }
        totalCpuGraph->update();
        std::map<int, std::pair<unsigned long long, unsigned long long>> currentTicks;
        for (ProcessInfo &process : processes) {
            const auto found = previousProcessTicks.find(process.pid);
            if (found != previousProcessTicks.end() && totalDelta > 0 && process.startTicks == found->second.first && process.ticks >= found->second.second)
                process.cpu = 100.0 * static_cast<double>(process.ticks - found->second.second) / totalDelta;
            currentTicks[process.pid] = {process.startTicks, process.ticks};
        }
        previousProcessTicks.swap(currentTicks);
        previousTotalTicks = currentTotal;
        previousIdleTicks = idle;
        const unsigned long long totalMemory = readMemoryValue("MemTotal:");
        const unsigned long long availableMemory = readMemoryValue("MemAvailable:");
        const double memory = totalMemory > availableMemory
            ? 100.0 * static_cast<double>(totalMemory - availableMemory) / totalMemory : 0.0;
        const unsigned long long swapTotal = readMemoryValue("SwapTotal:");
        const unsigned long long swapFree = readMemoryValue("SwapFree:");
        const unsigned long long swapUsed = swapTotal > swapFree ? swapTotal - swapFree : 0;
        const double paging = swapTotal > 0
            ? 100.0 * static_cast<double>(swapUsed) / static_cast<double>(swapTotal) : 0.0;
        const auto disk = readDiskBytes();
        const auto network = readNetworkBytes();
        const auto diskRate = previousDiskBytes && disk >= previousDiskBytes
            ? counterRate(disk, previousDiskBytes, sampleSeconds) : 0;
        const auto networkRate = previousNetworkBytes && network >= previousNetworkBytes
            ? counterRate(network, previousNetworkBytes, sampleSeconds) : 0;
        previousDiskBytes = disk;
        previousNetworkBytes = network;
        appendHistory(cpuHistory, cpu);
        appendHistory(memoryHistory, memory);
        appendHistory(swapHistory, paging);
        appendHistory(diskHistory, std::min(100.0, diskRate / (50.0 * 1024.0 * 1024.0) * 100.0));
        appendHistory(networkHistory, std::min(100.0, networkRate / (50.0 * 1024.0 * 1024.0) * 100.0));
        refreshProcesses(cpu);
        refreshApplications();
        refreshServices();
        refreshNetwork();
        refreshUsers();
        updateUptimeAndStatus(cpu, memory, paging, diskRate, networkRate);
        centralWidget()->update();
    }

    void applyTheme(const QString &theme) {
        themeName = theme;
        QString css;
        if (theme == "dark") {
            css = "QMainWindow,QWidget{background:#202326;color:#ededed;} QTabWidget::pane,QTableView,QTreeView{background:#292c30;color:#ededed;border:1px solid #50545a;} QHeaderView::section{background:#3a3e44;color:#fff;padding:4px;border:1px solid #555;} QPushButton,QComboBox{background:#373b40;color:#eee;border:1px solid #60656b;padding:3px 10px;} QLineEdit{background:#30343a;color:#fff;border:1px solid #666;padding:3px;} QGroupBox{border:1px solid #64686e;margin-top:8px;padding-top:8px;} QGroupBox::title{left:8px;padding:0 3px;} QTabBar::tab{background:#3a3e44;padding:5px 12px;border:1px solid #555;} QTabBar::tab:selected{background:#50565e;} QLabel#pageHeading{font-size:14pt;font-weight:bold;}";
        } else if (theme == "contrast") {
            css = "QMainWindow,QWidget{background:#000;color:#fff;} QTabWidget::pane,QTableView,QTreeView{background:#000;color:#fff;border:2px solid #fff;} QHeaderView::section,QPushButton{background:#000;color:#fff;border:2px solid #fff;padding:3px 10px;} QTabBar::tab{background:#000;color:#fff;border:2px solid #fff;padding:5px 12px;} QTabBar::tab:selected{background:#fff;color:#000;} QGroupBox{border:2px solid #fff;margin-top:8px;padding-top:8px;} QLabel#pageHeading{font-size:14pt;font-weight:bold;}";
        } else if (theme == "light") {
            css = "QMainWindow,QWidget{background:#f4f4f4;color:#202020;} QTabWidget::pane,QTableView,QTreeView{background:#fff;color:#202020;border:1px solid #9aa0a6;} QHeaderView::section{background:#e1e4e8;color:#202020;padding:4px;border:1px solid #b8bdc3;} QPushButton{background:#e8eaed;border:1px solid #858b91;padding:3px 10px;} QGroupBox{border:1px solid #9aa0a6;margin-top:8px;padding-top:8px;} QGroupBox::title{left:8px;padding:0 3px;} QTabBar::tab{background:#e1e4e8;padding:5px 12px;border:1px solid #b8bdc3;} QTabBar::tab:selected{background:#fff;} QLabel#pageHeading{font-size:14pt;font-weight:bold;}";
        } else {
            css = "QMainWindow,QWidget{background:#ece9d8;color:#111;} QTabWidget::pane,QTableView,QTreeView{background:#fff;color:#111;border:1px solid #9b978b;} QHeaderView::section{background:#d8d4c4;color:#111;padding:4px;border:1px solid #aaa69a;} QPushButton{background:#e6e2d3;color:#111;border:1px solid #7f7b70;padding:3px 12px;} QPushButton:hover{background:#f5f2e7;} QGroupBox{border:1px solid #9b978b;margin-top:8px;padding-top:8px;} QGroupBox::title{left:8px;padding:0 3px;} QTabBar::tab{background:#d8d4c4;padding:5px 12px;border:1px solid #aaa69a;} QTabBar::tab:selected{background:#fff;} QLabel#pageHeading{font-size:14pt;font-weight:bold;} QStatusBar{background:#d8d4c4;}";
        }
        const QString cardBackground = theme == "dark" ? "#292e34" : theme == "contrast" ? "#000000" : theme == "xp" ? "#f8f6ee" : "#ffffff";
        const QString border = theme == "dark" ? "#505a66" : theme == "contrast" ? "#ffffff" : "#b8b5a7";
        const QString muted = theme == "dark" ? "#b8c2ce" : theme == "contrast" ? "#ffffff" : "#555a61";
        const QString rxColor = theme == "dark" || theme == "contrast" ? "#77e3a1" : "#176739";
        const QString txColor = theme == "dark" || theme == "contrast" ? "#f3d96b" : "#745900";
        css += QString("QFrame#adapterCard{background:%1;border:1px solid %2;border-radius:9px;} "
                       "QFrame#adapterCard QLabel{background:transparent;} "
                       "QLabel#adapterName{font-size:12pt;font-weight:bold;} "
                       "QLabel#adapterRate{font-size:16pt;font-weight:bold;} "
                       "QLabel#adapterDetail{color:%3;font-size:9pt;} "
                       "QLabel#adapterState{border:1px solid %2;border-radius:8px;padding:2px 8px;} "
                       "QLabel#receiveCaption{color:%4;} QLabel#transmitCaption{color:%5;} "
                       "QPushButton#adapterCopy{border-radius:5px;padding:3px 8px;}")
                   .arg(cardBackground, border, muted, rxColor, txColor);
        css += QString("QLabel#graphHint{font-size:9pt;color:%1;} "
                       "QTreeView#networkList{border:0;background:%2;outline:0;} "
                       "QTreeView#networkList::item{border:0;padding:7px 8px;} "
                       "QTreeView#networkList QHeaderView::section{background:%2;color:%1;border:0;border-bottom:1px solid %3;padding:7px 8px;}")
                   .arg(muted, cardBackground, border);
        qApp->setStyleSheet(css);
        QSettings settings;
        settings.setValue("appearance/theme", themeName);
    }
};

} // namespace

#ifndef TASK_MANAGER_TEST
int main(int argc, char **argv) {
    QApplication application(argc, argv);
    application.setStyle(QStyleFactory::create("Fusion"));
    application.setOrganizationName("lintaskmanager");
    application.setApplicationName("lintaskmanager");
    application.setApplicationDisplayName("Linux Task Manager");
    application.setDesktopFileName("lintaskmanager");
    application.setWindowIcon(QIcon::fromTheme("lintaskmanager", QIcon(":/icons/lintaskmanager.png")));
    TaskManagerWindow window;
    window.show();
    return application.exec();
}

#endif
