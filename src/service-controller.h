#pragma once

#include <QtCore/QObject>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QRegularExpression>
#include <QtCore/QStandardPaths>
#include <functional>

namespace {

enum class ServiceOutcome { Success, Canceled, Failed };

// This runner never reads a password or invokes a shell. The desktop polkit
// agent owns authentication; only the fixed systemctl executable is elevated.
class ServiceController final : public QObject {
    friend class TaskManagerTests;
public:
    explicit ServiceController(QObject *parent = nullptr) : QObject(parent), command(this) {
        const QStringList systemDirectories = {"/usr/bin", "/bin", "/usr/sbin", "/sbin"};
        pkexecPath = QStandardPaths::findExecutable("pkexec", systemDirectories);
        systemctlPath = QStandardPaths::findExecutable("systemctl", systemDirectories);
        connect(&command, &QProcess::readyReadStandardOutput, this, [this] { drainOutput(); });
        connect(&command, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
            drainOutput();
            if (status == QProcess::CrashExit) {
                complete(ServiceOutcome::Failed, "The service command was interrupted. Its final state is unknown; check the refreshed service list.");
            } else if (code == 0) {
                complete(ServiceOutcome::Success, {});
            } else if (code == 126) {
                complete(ServiceOutcome::Canceled, "Authorization was canceled.");
            } else if (code == 127) {
                complete(ServiceOutcome::Failed,
                    "Authorization failed or the privileged command could not run. Check that a desktop polkit authentication agent is running.\n" + output.trimmed());
            } else {
                complete(ServiceOutcome::Failed, QString("systemctl failed (exit %1).\n%2").arg(code).arg(output.trimmed()));
            }
        });
        connect(&command, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                complete(ServiceOutcome::Failed, "Could not open administrator authorization: " + command.errorString());
        });
    }

    static bool validRequest(const QString &verb, const QString &unit) {
        static const QStringList verbs = {"start", "stop", "restart", "reload"};
        static const QRegularExpression unitPattern(R"(\A(?:[A-Za-z0-9:_.@-]|\\x[0-9a-fA-F]{2})+\.service\z)");
        return verbs.contains(verb) && unit.size() <= 255 && !unit.startsWith('-') && unitPattern.match(unit).hasMatch();
    }

    bool busy() const { return active; }
    bool available() const { return !pkexecPath.isEmpty() && !systemctlPath.isEmpty(); }
    QString errorText() const { return launchError; }
    std::function<void(ServiceOutcome, const QString &)> finished;

    bool run(const QString &verb, const QString &unit) {
        launchError.clear();
        if (active) { launchError = "Another service operation is still pending."; return false; }
        if (!validRequest(verb, unit)) { launchError = "Select a valid service and operation."; return false; }
        if (!available()) { launchError = "Service control requires systemctl, pkexec, and a running desktop polkit authentication agent."; return false; }
        output.clear();
        active = true;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert("LC_ALL", "C");
        command.setProcessEnvironment(environment);
        command.setProcessChannelMode(QProcess::MergedChannels);
        command.setStandardInputFile(QProcess::nullDevice());
        command.start(pkexecPath, {"--disable-internal-agent", systemctlPath,
            "--system", "--no-ask-password", verb, "--", unit});
        // Unlike discovery queries, authorization and systemd jobs must not be
        // killed after two seconds. systemctl waits for the job's actual result.
        return true;
    }

private:
    QProcess command;
    QString pkexecPath;
    QString systemctlPath;
    QString output;
    QString launchError;
    bool active = false;

    void drainOutput() {
        output += QString::fromLocal8Bit(command.readAllStandardOutput());
        if (output.size() > 16384) output = output.right(16384);
    }
    void complete(ServiceOutcome outcome, const QString &detail) {
        if (!active) return;
        active = false;
        if (finished) finished(outcome, detail);
    }
};

} // namespace
