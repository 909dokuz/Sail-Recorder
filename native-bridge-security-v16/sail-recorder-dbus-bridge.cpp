#include <QCoreApplication>
#include <QObject>
#include <QTimer>
#include <QDebug>
#include <QByteArray>
#include <QFile>
#include <QList>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusConnectionInterface>
#include <QtDBus/QDBusContext>
#include <QtDBus/QDBusError>
#include <QtDBus/QDBusReply>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <string>

class SailRecorderBridge : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.sailfishos.SailRecorder")

public:
    explicit SailRecorderBridge(QObject *parent = nullptr)
        : QObject(parent)
    {
        QTimer *timer = new QTimer(this);
        timer->setInterval(1500);

        connect(timer, &QTimer::timeout,
                this, &SailRecorderBridge::refresh);

        timer->start();

        QTimer::singleShot(0, this, &SailRecorderBridge::refresh);
    }

public slots:
    void toggle()
    {
        if (!authorizeToggleCaller()) {
            if (calledFromDBus()) {
                sendErrorReply(
                    QDBusError::AccessDenied,
                    "Sail Recorder control access denied"
                );
            }
            return;
        }

        if (m_busy)
            return;

        // Her tiklamada daemon'in GERCEK durumunu oku.
        // Bellekteki state eski kalmis olsa bile Start/Stop dogru secilsin.
        bool ok = false;
        const std::string body = socketCommand("status", ok);

        if (ok)
            m_recording = body.find("Hazir") == std::string::npos;

        command(m_recording ? "stop" : "start");
    }

    bool status()
    {
        bool ok = false;
        const std::string body = socketCommand("status", ok);

        if (ok)
            m_recording = body.find("Hazir") == std::string::npos;

        return m_recording;
    }

    void refresh()
    {
        if (m_busy)
            return;

        bool ok = false;
        const std::string body = socketCommand("status", ok);

        if (ok)
            m_recording = body.find("Hazir") == std::string::npos;

        emit stateChanged(m_recording);
    }

signals:
    void stateChanged(bool state);

private:
    static bool readProcFile(
        uint pid,
        const char *name,
        QByteArray &data
    )
    {
        const QString path =
            QString("/proc/%1/%2")
                .arg(pid)
                .arg(QString::fromLatin1(name));

        QFile file(path);

        if (!file.open(QIODevice::ReadOnly))
            return false;

        data = file.readAll();
        return true;
    }

    static bool processExe(uint pid, QByteArray &exe)
    {
        const QByteArray path =
            QByteArray("/proc/") +
            QByteArray::number(pid) +
            "/exe";

        char buffer[4096];

        const ssize_t size =
            ::readlink(
                path.constData(),
                buffer,
                sizeof(buffer) - 1
            );

        if (size <= 0 ||
            size >= static_cast<ssize_t>(sizeof(buffer))) {
            return false;
        }

        buffer[size] = '\0';
        exe = QByteArray(buffer, static_cast<int>(size));
        return true;
    }

    static bool processParentPid(uint pid, uint &parentPid)
    {
        QByteArray status;

        if (!readProcFile(pid, "status", status))
            return false;

        const QList<QByteArray> lines = status.split('\n');

        for (const QByteArray &line : lines) {
            if (!line.startsWith("PPid:"))
                continue;

            bool ok = false;

            const uint value =
                line.mid(5).trimmed().toUInt(&ok);

            if (!ok || value <= 1)
                return false;

            parentPid = value;
            return true;
        }

        return false;
    }

    static bool processCgroupEndsWith(
        uint pid,
        const QByteArray &suffix
    )
    {
        QByteArray cgroup;

        if (!readProcFile(pid, "cgroup", cgroup))
            return false;

        const QList<QByteArray> lines = cgroup.split('\n');

        for (const QByteArray &line : lines) {
            const int colon = line.lastIndexOf(':');

            if (colon < 0)
                continue;

            if (line.mid(colon + 1).endsWith(suffix))
                return true;
        }

        return false;
    }

    static bool processHasArg(
        uint pid,
        const QByteArray &expected
    )
    {
        QByteArray cmdline;

        if (!readProcFile(pid, "cmdline", cmdline))
            return false;

        const QList<QByteArray> args = cmdline.split('\0');

        for (const QByteArray &arg : args) {
            if (arg == expected)
                return true;
        }

        return false;
    }

    static bool isLipstickCaller(uint pid)
    {
        QByteArray status;

        if (!readProcFile(pid, "status", status))
            return false;

        bool lipstickName = false;

        const QList<QByteArray> statusLines =
            status.split('\n');

        for (const QByteArray &line : statusLines) {
            if (!line.startsWith("Name:"))
                continue;

            lipstickName =
                line.mid(5).trimmed() == "lipstick";
            break;
        }

        if (!lipstickName)
            return false;

        QByteArray cmdline;

        if (!readProcFile(pid, "cmdline", cmdline))
            return false;

        const QByteArray expectedArg0("/usr/bin/lipstick");

        if (cmdline.size() <= expectedArg0.size() ||
            !cmdline.startsWith(expectedArg0) ||
            cmdline.at(expectedArg0.size()) != '\0') {
            return false;
        }

        return processCgroupEndsWith(
            pid,
            "/lipstick.service"
        );
    }

    static bool isSailRecorderAppCaller(uint pid)
    {
        QByteArray exe;

        if (!processExe(pid, exe))
            return false;

        if (exe != "/usr/bin/xdg-dbus-proxy")
            return false;

        uint parentPid = 0;

        if (!processParentPid(pid, parentPid))
            return false;

        QByteArray parentStatus;

        if (!readProcFile(parentPid, "status", parentStatus))
            return false;

        bool firejailName = false;

        const QList<QByteArray> statusLines =
            parentStatus.split('\n');

        for (const QByteArray &line : statusLines) {
            if (!line.startsWith("Name:"))
                continue;

            firejailName =
                line.mid(5).trimmed() == "firejail";
            break;
        }

        if (!firejailName)
            return false;

        QByteArray parentCmdline;

        if (!readProcFile(parentPid, "cmdline", parentCmdline))
            return false;

        const QByteArray expectedArg0("/usr/bin/firejail");

        if (parentCmdline.size() <= expectedArg0.size() ||
            !parentCmdline.startsWith(expectedArg0) ||
            parentCmdline.at(expectedArg0.size()) != '\0') {
            return false;
        }

        const char *requiredArgs[] = {
            "--template=OrganizationName:org.sailrecorder",
            "--template=ApplicationName:SailRecorder",
            "--profile=/etc/sailjail/permissions/sail-recorder.profile",
            "/usr/bin/qmlscene",
            "/usr/share/sail-recorder/qml/ScreenKayitClean.qml"
        };

        for (const char *arg : requiredArgs) {
            if (!processHasArg(
                    parentPid,
                    QByteArray(arg))) {
                return false;
            }
        }

        return true;
    }

    bool authorizeToggleCaller()
    {
        if (!calledFromDBus()) {
            qWarning()
                << "DBUS AUTH DENY"
                << "reason=not-dbus";
            return false;
        }

        const QString sender = message().service();

        if (sender.isEmpty()) {
            qWarning()
                << "DBUS AUTH DENY"
                << "reason=empty-sender";
            return false;
        }

        QDBusConnectionInterface *iface =
            connection().interface();

        if (!iface) {
            qWarning()
                << "DBUS AUTH DENY"
                << "reason=no-bus-interface";
            return false;
        }

        const QDBusReply<uint> pidReply =
            iface->servicePid(sender);

        const QDBusReply<uint> uidReply =
            iface->serviceUid(sender);

        if (!pidReply.isValid() ||
            !uidReply.isValid()) {
            qWarning()
                << "DBUS AUTH DENY"
                << "reason=credential-query"
                << "sender=" << sender;
            return false;
        }

        const uint pid = pidReply.value();
        const uint uid = uidReply.value();

        if (pid <= 1 ||
            uid != static_cast<uint>(::getuid())) {
            qWarning()
                << "DBUS AUTH DENY"
                << "reason=uid-pid"
                << "sender=" << sender
                << "pid=" << pid
                << "uid=" << uid;
            return false;
        }

        const bool lipstick =
            isLipstickCaller(pid);

        const bool sailRecorder =
            !lipstick &&
            isSailRecorderAppCaller(pid);

        if (!lipstick && !sailRecorder) {
            qWarning()
                << "DBUS AUTH DENY"
                << "reason=untrusted-process"
                << "sender=" << sender
                << "pid=" << pid;
            return false;
        }

        // Sender -> PID eslesmesini kontrolden sonra tekrar dogrula.
        const QDBusReply<uint> confirmPid =
            iface->servicePid(sender);

        if (!confirmPid.isValid() ||
            confirmPid.value() != pid) {
            qWarning()
                << "DBUS AUTH DENY"
                << "reason=pid-changed"
                << "sender=" << sender;
            return false;
        }

        qInfo()
            << "DBUS AUTH ALLOW"
            << (lipstick ? "lipstick" : "sail-recorder")
            << "sender=" << sender
            << "pid=" << pid;

        return true;
    }

    bool m_recording = false;
    bool m_busy = false;

    static std::string socketCommand(const char *path, bool &ok)
    {
        ok = false;

        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return {};

        struct timeval timeout;
        timeout.tv_sec = 3;
        timeout.tv_usec = 0;

        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                     &timeout, sizeof(timeout));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                     &timeout, sizeof(timeout));

        const QByteArray runtimeDir = qgetenv("XDG_RUNTIME_DIR");
        if (runtimeDir.isEmpty()) {
            ::close(fd);
            return {};
        }

        const QByteArray socketPath =
            runtimeDir + "/sail-recorder-control.sock";

        sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;

        if (socketPath.size() >=
                static_cast<int>(sizeof(addr.sun_path))) {
            ::close(fd);
            return {};
        }

        std::strncpy(
            addr.sun_path,
            socketPath.constData(),
            sizeof(addr.sun_path) - 1
        );

        if (::connect(fd,
                      reinterpret_cast<sockaddr *>(&addr),
                      sizeof(addr)) != 0) {
            ::close(fd);
            return {};
        }

        std::string request = std::string(path) + "\n";

        const char *ptr = request.data();
        size_t remaining = request.size();

        while (remaining > 0) {
            ssize_t n = ::send(fd, ptr, remaining, 0);
            if (n <= 0) {
                ::close(fd);
                return {};
            }

            ptr += n;
            remaining -= static_cast<size_t>(n);
        }

        std::string response;
        char buffer[4096];

        while (response.size() < 65536) {
            ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);

            if (n == 0)
                break;

            if (n < 0) {
                ::close(fd);
                return {};
            }

            response.append(buffer, static_cast<size_t>(n));
        }

        ::close(fd);

        if (response.empty())
            return {};

        while (!response.empty() &&
               (response.back() == '\n' ||
                response.back() == '\r')) {
            response.pop_back();
        }

        ok = true;
        return response;
    }

    void command(const char *path)
    {
        m_busy = true;

        bool ok = false;
        const std::string reply = socketCommand(path, ok);

        qInfo() << "Sail Recorder" << path
                << "ok=" << ok
                << "reply=" << QString::fromStdString(reply);

        m_busy = false;

        // D-Bus method reply once donsun; stateChanged sonraki event-loop
        // turunda yayinlansin. Top Menu busy durumunda takilmasin.
        QTimer::singleShot(0, this, &SailRecorderBridge::refresh);
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QDBusConnection bus = QDBusConnection::sessionBus();

    if (!bus.isConnected()) {
        qCritical() << "Session D-Bus connection failed";
        return 2;
    }

    if (!bus.registerService("org.sailfishos.SailRecorder")) {
        qCritical() << "Cannot register D-Bus service:"
                    << bus.lastError().message();
        return 3;
    }

    SailRecorderBridge bridge;

    if (!bus.registerObject(
            "/org/sailfishos/SailRecorder",
            &bridge,
            QDBusConnection::ExportAllSlots |
            QDBusConnection::ExportAllSignals)) {
        qCritical() << "Cannot register D-Bus object:"
                    << bus.lastError().message();
        return 4;
    }

    return app.exec();
}

#include "sail-recorder-dbus-bridge.moc"
