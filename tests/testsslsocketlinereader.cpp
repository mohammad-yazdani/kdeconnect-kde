/**
 * Copyright 2015 Vineet Garg <albertvaka@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License or (at your option) version 3 or any later version
 * accepted by the membership of KDE e.V. (or its successor approved
 * by the membership of KDE e.V.), which shall act as a proxy
 * defined in Section 14 of version 3 of the license.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "../core/backends/lan/server.h"
#include "../core/backends/lan/socketlinereader.h"
#include "../core/qtcompat_p.h"

#include <QSslKey>
#include <QtCrypto>
#include <QTest>
#include <QTimer>
#include <QSignalSpy>

/*
 * This class tests the behaviour of socket line reader when the connection if over ssl. Since SSL takes part below application layer,
 * working of SocketLineReader should be same.
 */
class TestSslSocketLineReader : public QObject
{
    Q_OBJECT
public Q_SLOTS:
    void newPacket();
    void testTimeout();

private Q_SLOTS:

    void initTestCase();
    void init();
    void cleanup();
    void cleanupTestCase();

    void testTrustedDevice();
    void testUntrustedDevice();
    void testTrustedDeviceWithWrongCertificate();


private:
    const int PORT = 7894;
    const int TIMEOUT = 4 * 1000;
    QTimer m_timer;
    QCA::Initializer m_qcaInitializer;
    QEventLoop m_loop;
    QList<QByteArray> m_packets;
    Server* m_server;
    QSslSocket* m_clientSocket;
    SocketLineReader* m_reader;

private:
    void setSocketAttributes(QSslSocket* socket, QString deviceName);
};

void TestSslSocketLineReader::initTestCase()
{
    m_server = new Server(this);

    QVERIFY2(m_server->listen(QHostAddress::LocalHost, PORT), "Failed to create local tcp server");

    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &TestSslSocketLineReader::testTimeout);
}

void TestSslSocketLineReader::init()
{
    m_timer.setInterval(TIMEOUT);
    m_timer.start();

    m_clientSocket = new QSslSocket(this);
    m_clientSocket->connectToHost(QHostAddress::LocalHost, PORT);
    connect(m_clientSocket, &QAbstractSocket::connected, &m_loop, &QEventLoop::quit);

    m_loop.processEvents(QEventLoop::AllEvents, TIMEOUT);

    QVERIFY2(m_clientSocket->isOpen(), "Could not connect to local tcp server");
}

void TestSslSocketLineReader::cleanup()
{
    m_clientSocket->disconnectFromHost();
    m_timer.stop();
    delete m_clientSocket;
}

void TestSslSocketLineReader::cleanupTestCase()
{
    delete m_server;
}

void TestSslSocketLineReader::testTrustedDevice()
{

    int maxAttemps = 5;
    while(!m_server->hasPendingConnections() && maxAttemps > 0) {
        --maxAttemps;
        QTest::qSleep(1000);
    }
    QCOMPARE(true, m_server->hasPendingConnections());

    QSslSocket* serverSocket = m_server->nextPendingConnection();

    QSignalSpy serverEncryptedSpy(serverSocket, SIGNAL(encrypted()));

    QVERIFY2(serverSocket != 0, "Null socket returned by server");
    QVERIFY2(serverSocket->isOpen(), "Server socket already closed");

    setSocketAttributes(serverSocket, QStringLiteral("Test Server"));
    setSocketAttributes(m_clientSocket, QStringLiteral("Test Client"));

    serverSocket->setPeerVerifyName(QStringLiteral("Test Client"));
    serverSocket->setPeerVerifyMode(QSslSocket::VerifyPeer);
    serverSocket->addCaCertificate(m_clientSocket->localCertificate());

    m_clientSocket->setPeerVerifyName(QStringLiteral("Test Server"));
    m_clientSocket->setPeerVerifyMode(QSslSocket::VerifyPeer);
    m_clientSocket->addCaCertificate(serverSocket->localCertificate());

    int connected_sockets = 0;
    auto connected_lambda = [&](){
        connected_sockets++;
        if (connected_sockets >= 2) {
            m_loop.quit();
        }
    };
    connect(serverSocket, &QSslSocket::encrypted, connected_lambda);
    connect(m_clientSocket, &QSslSocket::encrypted, connected_lambda);

    serverSocket->startServerEncryption();
    m_clientSocket->startClientEncryption();
    m_loop.exec(); //Block until QEventLoop::quit gets called by the lambda

    if (serverEncryptedSpy.count() < 1) {
        for(int x = 0;x < 50; ++x) {
            QVERIFY(serverEncryptedSpy.wait(100));
            if (serverEncryptedSpy.count() > 0) {
                break;
            }
        }
    }

    // Both client and server socket should be encrypted here and should have remote certificate because VerifyPeer is used
    QVERIFY2(m_clientSocket->isOpen(), "Client socket already closed");
    QVERIFY2(serverSocket->isOpen(), "Server socket already closed");
    QVERIFY2(m_clientSocket->isEncrypted(), "Client is not encrypted");
    QVERIFY2(serverSocket->isEncrypted(), "Server is not encrypted");
    QVERIFY2(!m_clientSocket->peerCertificate().isNull(), "Server certificate not received");
    QVERIFY2(!serverSocket->peerCertificate().isNull(), "Client certificate not received");

    QList<QByteArray> dataToSend;
    dataToSend << "foobar\n" << "barfoo\n" << "foobar?\n" << "\n" << "barfoo!\n" << "panda\n";
    for (const QByteArray& line : qAsConst(dataToSend)) {
        m_clientSocket->write(line);
    }
    m_clientSocket->flush();

    m_packets.clear();

    m_reader = new SocketLineReader(serverSocket, this);
    connect(m_reader, &SocketLineReader::readyRead, this,&TestSslSocketLineReader::newPacket);
    m_loop.exec();

    /* remove the empty line before compare */
    dataToSend.removeOne("\n");

    QCOMPARE(m_packets.count(), 5);//We expect 5 Packets
    for(int x = 0;x < 5; ++x) {
        QCOMPARE(m_packets[x], dataToSend[x]);
    }

    delete m_reader;
}

void TestSslSocketLineReader::testUntrustedDevice()
{
    int maxAttemps = 5;
    while(!m_server->hasPendingConnections() && maxAttemps > 0) {
        --maxAttemps;
        QTest::qSleep(1000);
    }
    QCOMPARE(true, m_server->hasPendingConnections());

    QSslSocket* serverSocket = m_server->nextPendingConnection();

    QSignalSpy serverEncryptedSpy(serverSocket, SIGNAL(encrypted()));

    QVERIFY2(serverSocket != 0, "Null socket returned by server");
    QVERIFY2(serverSocket->isOpen(), "Server socket already closed");

    setSocketAttributes(serverSocket, QStringLiteral("Test Server"));
    setSocketAttributes(m_clientSocket, QStringLiteral("Test Client"));

    serverSocket->setPeerVerifyName(QStringLiteral("Test Client"));
    serverSocket->setPeerVerifyMode(QSslSocket::QueryPeer);

    m_clientSocket->setPeerVerifyName(QStringLiteral("Test Server"));
    m_clientSocket->setPeerVerifyMode(QSslSocket::QueryPeer);

    int connected_sockets = 0;
    auto connected_lambda = [&](){
        connected_sockets++;
        if (connected_sockets >= 2) {
            m_loop.quit();
        }
    };
    connect(serverSocket, &QSslSocket::encrypted, connected_lambda);
    connect(m_clientSocket, &QSslSocket::encrypted, connected_lambda);
    serverSocket->startServerEncryption();
    m_clientSocket->startClientEncryption();
    m_loop.exec(); //Block until QEventLoop::quit gets called by the lambda

    if (serverEncryptedSpy.count() < 1) {
        for(int x = 0;x < 50; ++x) {
            QVERIFY(serverEncryptedSpy.wait(100));
            if (serverEncryptedSpy.count() > 0) {
                break;
            }
        }
    }

    QVERIFY2(m_clientSocket->isOpen(), "Client socket already closed");
    QVERIFY2(serverSocket->isOpen(), "Server socket already closed");
    QVERIFY2(m_clientSocket->isEncrypted(), "Client is not encrypted");
    QVERIFY2(serverSocket->isEncrypted(), "Server is not encrypted");
    QVERIFY2(!m_clientSocket->peerCertificate().isNull(), "Server certificate not received");
    QVERIFY2(!serverSocket->peerCertificate().isNull(), "Client certificate not received");

    QList<QByteArray> dataToSend;
    dataToSend << "foobar\n" << "barfoo\n" << "foobar?\n" << "\n" << "barfoo!\n" << "panda\n";
    for (const QByteArray& line : qAsConst(dataToSend)) {
            m_clientSocket->write(line);
        }
    m_clientSocket->flush();

    m_packets.clear();

    m_reader = new SocketLineReader(serverSocket, this);
    connect(m_reader, &SocketLineReader::readyRead, this, &TestSslSocketLineReader::newPacket);
    m_loop.exec();

    /* remove the empty line before compare */
    dataToSend.removeOne("\n");

    QCOMPARE(m_packets.count(), 5);//We expect 5 Packets
    for(int x = 0;x < 5; ++x) {
        QCOMPARE(m_packets[x], dataToSend[x]);
    }

    delete m_reader;
}

void TestSslSocketLineReader::testTrustedDeviceWithWrongCertificate()
{
    int maxAttemps = 5;
    while(!m_server->hasPendingConnections() && maxAttemps > 0) {
        --maxAttemps;
        QTest::qSleep(1000);
    }
    QCOMPARE(true, m_server->hasPendingConnections());

    QSslSocket* serverSocket = m_server->nextPendingConnection();

    QVERIFY2(serverSocket != 0, "Could not open a connection to the client");

    setSocketAttributes(serverSocket, QStringLiteral("Test Server"));
    setSocketAttributes(m_clientSocket, QStringLiteral("Test Client"));

    // Not adding other device certificate to list of CA certificate, and using VerifyPeer. This should lead to handshake failure
    serverSocket->setPeerVerifyName(QStringLiteral("Test Client"));
    serverSocket->setPeerVerifyMode(QSslSocket::VerifyPeer);

    m_clientSocket->setPeerVerifyName(QStringLiteral("Test Server"));
    m_clientSocket->setPeerVerifyMode(QSslSocket::VerifyPeer);

    connect(serverSocket, &QSslSocket::encrypted, &m_loop, &QEventLoop::quit); // Encrypted signal should never be emitted
    connect(m_clientSocket, &QSslSocket::encrypted, &m_loop, &QEventLoop::quit); // Encrypted signal should never be emitted
    connect(serverSocket, &QAbstractSocket::disconnected, &m_loop, &QEventLoop::quit);
    connect(m_clientSocket, &QAbstractSocket::disconnected, &m_loop, &QEventLoop::quit);

    serverSocket->startServerEncryption();
    m_clientSocket->startClientEncryption();
    m_loop.exec();

    QVERIFY2(!serverSocket->isEncrypted(), "Server is encrypted, it should not");
    QVERIFY2(!m_clientSocket->isEncrypted(), "lient is encrypted, it should now");

    if (serverSocket->state() != QAbstractSocket::UnconnectedState) m_loop.exec(); // Wait until serverSocket is disconnected, It should be in disconnected state
    if (m_clientSocket->state() != QAbstractSocket::UnconnectedState) m_loop.exec(); // Wait until mClientSocket is disconnected, It should be in disconnected state

    QCOMPARE((int)m_clientSocket->state(), 0);
    QCOMPARE((int)serverSocket->state(), 0);

}

void TestSslSocketLineReader::newPacket()
{
    if (!m_reader->bytesAvailable()) {
        return;
    }

    int maxLoops = 5;
    while(m_reader->bytesAvailable() > 0 && maxLoops > 0) {
        --maxLoops;
        const QByteArray packet = m_reader->readLine();
        if (!packet.isEmpty()) {
            m_packets.append(packet);
        }

        if (m_packets.count() == 5) {
            m_loop.exit();
        }
    }
}

void TestSslSocketLineReader::testTimeout()
{
    m_loop.exit(-1);
    QFAIL("Test Timed Out");
}

void TestSslSocketLineReader::setSocketAttributes(QSslSocket* socket, QString deviceName) {

    QDateTime startTime = QDateTime::currentDateTime();
    QDateTime endTime = startTime.addYears(10);
    QCA::CertificateInfo certificateInfo;
    certificateInfo.insert(QCA::CommonName,deviceName);
    certificateInfo.insert(QCA::Organization,QStringLiteral("KDE"));
    certificateInfo.insert(QCA::OrganizationalUnit,QStringLiteral("Kde connect"));

    QCA::CertificateOptions certificateOptions(QCA::PKCS10);
    certificateOptions.setSerialNumber(10);
    certificateOptions.setInfo(certificateInfo);
    certificateOptions.setValidityPeriod(startTime, endTime);
    certificateOptions.setFormat(QCA::PKCS10);

    QCA::PrivateKey privKey = QCA::KeyGenerator().createRSA(2048);
    QSslCertificate certificate = QSslCertificate(QCA::Certificate(certificateOptions, privKey).toPEM().toLatin1());

    socket->setPrivateKey(QSslKey(privKey.toPEM().toLatin1(), QSsl::Rsa));
    socket->setLocalCertificate(certificate);

}

QTEST_GUILESS_MAIN(TestSslSocketLineReader)

#include "testsslsocketlinereader.moc"

