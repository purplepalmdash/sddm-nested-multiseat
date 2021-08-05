/*
 * Main authentication application class
 * Copyright (C) 2013 Martin Bříza <mbriza@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include "HelperApp.h"
#include "Backend.h"
#include "UserSession.h"
#include "SafeDataStream.h"

#include "MessageHandler.h"
#include "VirtualTerminal.h"

#include <QtCore/QTimer>
#include <QtCore/QFile>
#include <QtCore/QDebug>
#include <QtNetwork/QLocalSocket>

#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>

#if defined(Q_OS_LINUX)
#include <utmp.h>
#endif
#include <utmpx.h>
#include <QByteArray>

namespace SDDM {
    HelperApp::HelperApp(int& argc, char** argv)
            : QCoreApplication(argc, argv)
            , m_backend(Backend::get(this))
            , m_session(new UserSession(this))
            , m_socket(new QLocalSocket(this)) {
        qInstallMessageHandler(HelperMessageHandler);

        QTimer::singleShot(0, this, SLOT(setUp()));
    }

    void HelperApp::setUp() {
        QStringList args = QCoreApplication::arguments();
        QString server;
        int pos;

        if ((pos = args.indexOf(QStringLiteral("--socket"))) >= 0) {
            if (pos >= args.length() - 1) {
                qCritical() << "This application is not supposed to be executed manually";
                exit(Auth::HELPER_OTHER_ERROR);
                return;
            }
            server = args[pos + 1];
        }

        if ((pos = args.indexOf(QStringLiteral("--id"))) >= 0) {
            if (pos >= args.length() - 1) {
                qCritical() << "This application is not supposed to be executed manually";
                exit(Auth::HELPER_OTHER_ERROR);
                return;
            }
            m_id = QString(args[pos + 1]).toLongLong();
        }

        if ((pos = args.indexOf(QStringLiteral("--start"))) >= 0) {
            if (pos >= args.length() - 1) {
                qCritical() << "This application is not supposed to be executed manually";
                exit(Auth::HELPER_OTHER_ERROR);
                return;
            }
            m_session->setPath(args[pos + 1]);
        }

        if ((pos = args.indexOf(QStringLiteral("--user"))) >= 0) {
            if (pos >= args.length() - 1) {
                qCritical() << "This application is not supposed to be executed manually";
                exit(Auth::HELPER_OTHER_ERROR);
                return;
            }
            m_user = args[pos + 1];
        }

        if ((pos = args.indexOf(QStringLiteral("--autologin"))) >= 0) {
            m_backend->setAutologin(true);
        }

        if ((pos = args.indexOf(QStringLiteral("--greeter"))) >= 0) {
            m_backend->setGreeter(true);
        }

        if (server.isEmpty() || m_id <= 0) {
            qCritical() << "This application is not supposed to be executed manually";
            exit(Auth::HELPER_OTHER_ERROR);
            return;
        }

        connect(m_socket, &QLocalSocket::connected, this, &HelperApp::doAuth);
        connect(m_session, QOverload<int>::of(&QProcess::finished), this, &HelperApp::sessionFinished);
        m_socket->connectToServer(server, QIODevice::ReadWrite | QIODevice::Unbuffered);
    }

    void HelperApp::doAuth() {
        SafeDataStream str(m_socket);
        str << Msg::HELLO << m_id;
        str.send();
        if (str.status() != QDataStream::Ok)
            qCritical() << "Couldn't write initial message:" << str.status();

        if (!m_backend->start(m_user)) {
            authenticated(QString());

            // write failed login to btmp
            QProcessEnvironment env = m_session->processEnvironment();
            QString displayId = env.value(QStringLiteral("DISPLAY"));
            QString vt = env.value(QStringLiteral("XDG_VTNR"));
            utmpLogin(vt, displayId, m_user, 0, false);

            exit(Auth::HELPER_AUTH_ERROR);
            return;
        }

        if (!m_backend->authenticate()) {
            authenticated(QString());

            // write failed login to btmp
            QProcessEnvironment env = m_session->processEnvironment();
            QString displayId = env.value(QStringLiteral("DISPLAY"));
            QString vt = env.value(QStringLiteral("XDG_VTNR"));
            utmpLogin(vt, displayId, m_user, 0, false);

            exit(Auth::HELPER_AUTH_ERROR);
            return;
        }

        m_user = m_backend->userName();
        QProcessEnvironment env = authenticated(m_user);

        if (!m_session->path().isEmpty()) {
            env.insert(m_session->processEnvironment());
            // Allocate a new VT for the wayland session
            if(env.value(QStringLiteral("XDG_SESSION_TYPE")) == QLatin1String("wayland")) {
                int vtNumber = VirtualTerminal::setUpNewVt();
                env.insert(QStringLiteral("XDG_VTNR"), QString::number(vtNumber));
            }
            m_session->setProcessEnvironment(env);

            if (!m_backend->openSession()) {
                sessionOpened(false);
                exit(Auth::HELPER_SESSION_ERROR);
                return;
            }

            sessionOpened(true);

            // write successful login to utmp/wtmp
            QProcessEnvironment env = m_session->processEnvironment();
            QString displayId = env.value(QStringLiteral("DISPLAY"));
            QString vt = env.value(QStringLiteral("XDG_VTNR"));
            if (env.value(QStringLiteral("XDG_SESSION_CLASS")) != QLatin1String("greeter")) {
                // cache pid for session end
                m_session->setCachedProcessId(m_session->processId());
                utmpLogin(vt, displayId, m_user, m_session->processId(), true);
            }
        }
        else
            exit(Auth::HELPER_SUCCESS);
        return;
    }

    void HelperApp::sessionFinished(int status) {
        m_backend->closeSession();

        // write logout to utmp/wtmp
        qint64 pid = m_session->cachedProcessId();
        QProcessEnvironment env = m_session->processEnvironment();
        if (env.value(QStringLiteral("XDG_SESSION_CLASS")) != QLatin1String("greeter")) {
            QString vt = env.value(QStringLiteral("XDG_VTNR"));
            QString displayId = env.value(QStringLiteral("DISPLAY"));
            utmpLogout(vt, displayId, pid);
        }

        exit(status);
    }

    void HelperApp::info(const QString& message, Auth::Info type) {
        SafeDataStream str(m_socket);
        str << Msg::INFO << message << type;
        str.send();
        m_socket->waitForBytesWritten();
    }

    void HelperApp::error(const QString& message, Auth::Error type) {
        SafeDataStream str(m_socket);
        str << Msg::ERROR << message << type;
        str.send();
        m_socket->waitForBytesWritten();
    }

    Request HelperApp::request(const Request& request) {
        Msg m = Msg::MSG_UNKNOWN;
        Request response;
        SafeDataStream str(m_socket);
        str << Msg::REQUEST << request;
        str.send();
        str.receive();
        str >> m >> response;
        if (m != REQUEST) {
            response = Request();
            qCritical() << "Received a wrong opcode instead of REQUEST:" << m;
        }
        return response;
    }

    QProcessEnvironment HelperApp::authenticated(const QString &user) {
        Msg m = Msg::MSG_UNKNOWN;
        QProcessEnvironment env;
        SafeDataStream str(m_socket);
        str << Msg::AUTHENTICATED << user;
        str.send();
        if (user.isEmpty())
            return env;
        str.receive();
        str >> m >> env >> m_cookie;
        if (m != AUTHENTICATED) {
            env = QProcessEnvironment();
            m_cookie = QString();
            qCritical() << "Received a wrong opcode instead of AUTHENTICATED:" << m;
        }
        return env;
    }

    void HelperApp::sessionOpened(bool success) {
        Msg m = Msg::MSG_UNKNOWN;
        SafeDataStream str(m_socket);
        str << Msg::SESSION_STATUS << success;
        str.send();
        str.receive();
        str >> m;
        if (m != SESSION_STATUS) {
            qCritical() << "Received a wrong opcode instead of SESSION_STATUS:" << m;
        }
    }

    UserSession *HelperApp::session() {
        return m_session;
    }

    const QString& HelperApp::user() const {
        return m_user;
    }

    const QString& HelperApp::cookie() const {
        return m_cookie;
    }

    HelperApp::~HelperApp() {

    }

    void HelperApp::utmpLogin(const QString &vt, const QString &displayName, const QString &user, qint64 pid, bool authSuccessful) {
        struct utmpx entry;
        struct timeval tv;

        entry = { 0 };
        entry.ut_type = USER_PROCESS;
        entry.ut_pid = pid;

        // ut_line: vt
        if (!vt.isEmpty()) {
            QString tty = QStringLiteral("tty");
            tty.append(vt);
            QByteArray ttyBa = tty.toLocal8Bit();
            const char* ttyChar = ttyBa.constData();
            strncpy(entry.ut_line, ttyChar, sizeof(entry.ut_line) - 1);
        }

        // ut_host: displayName
        QByteArray displayBa = displayName.toLocal8Bit();
        const char* displayChar = displayBa.constData();
        strncpy(entry.ut_host, displayChar, sizeof(entry.ut_host) - 1);

        // ut_user: user
        QByteArray userBa = user.toLocal8Bit();
        const char* userChar = userBa.constData();
        strncpy(entry.ut_user, userChar, sizeof(entry.ut_user) -1);

        gettimeofday(&tv, NULL);
        entry.ut_tv.tv_sec = tv.tv_sec;
        entry.ut_tv.tv_usec = tv.tv_usec;

        // write to utmp
        setutxent();
        if (!pututxline (&entry))
            qWarning() << "Failed to write utmpx: " << strerror(errno);
        endutxent();

#if !defined(Q_OS_FREEBSD)
        // append to failed login database btmp
        if (!authSuccessful) {
#if defined(Q_OS_LINUX)
            updwtmpx("/var/log/btmp", &entry);
#endif
        }

        // append to wtmp
        else {
#if defined(Q_OS_LINUX)
            updwtmpx("/var/log/wtmp", &entry);
#endif
        }
#endif
    }

    void HelperApp::utmpLogout(const QString &vt, const QString &displayName, qint64 pid) {
        struct utmpx entry;
        struct timeval tv;

        entry = { 0 };
        entry.ut_type = DEAD_PROCESS;
        entry.ut_pid = pid;

        // ut_line: vt
        if (!vt.isEmpty()) {
            QString tty = QStringLiteral("tty");
            tty.append(vt);
            QByteArray ttyBa = tty.toLocal8Bit();
            const char* ttyChar = ttyBa.constData();
            strncpy(entry.ut_line, ttyChar, sizeof(entry.ut_line) - 1);
        }

        // ut_host: displayName
        QByteArray displayBa = displayName.toLocal8Bit();
        const char* displayChar = displayBa.constData();
        strncpy(entry.ut_host, displayChar, sizeof(entry.ut_host) - 1);

        gettimeofday(&tv, NULL);
        entry.ut_tv.tv_sec = tv.tv_sec;
        entry.ut_tv.tv_usec = tv.tv_usec;

        // write to utmp
        setutxent();
        if (!pututxline (&entry))
            qWarning() << "Failed to write utmpx: " << strerror(errno);
        endutxent();

#if defined(Q_OS_LINUX)
        // append to wtmp
        updwtmpx("/var/log/wtmp", &entry);
#elif defined(Q_OS_FREEBSD)
        pututxline(&entry);
#endif
    }
}

int main(int argc, char** argv) {
    SDDM::HelperApp app(argc, argv);
    return app.exec();
}
