#pragma once

#include <QObject>
#include <QLocalSocket>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QImage>
#include <QCache>
#include <QNetworkAccessManager>

class QDiscord : public QObject {
Q_OBJECT

public:
	QDiscord();

public:
	/**
	 * Tries to connext to the Discord. Returns true if successfull (this function is blocking)
	 */
	bool connect(const QString &clientID, const QString &clientSecret);

	void disconnect();

	inline bool isConnected() const {
		return isConnected_;
	}

	inline const QString &userID() const {
		return userID_;
	}

public:
	/**
	 * Sends a command and blocking calls for result.
	 * $args is put in the "args" field.
	 * $msgOverrides is injected into the main body
	 */
	QJsonObject sendCommand(const QString &command, const QJsonObject &args, const QJsonObject &msgOverrides = {});

public:
	/// The function can be async, the avatar loading can be delayed and then signalled using avatarReady
	QImage getUserAvatar(const QString &userId, const QString &avatarId);

signals:
	/// This signal is emitted when there is a message received that is not a response to a command
	void messageReceived(const QJsonObject &msg);

	void avatarReady(const QString &avatarId, const QImage &img);

	void connected();

	void disconnected();

private:
	/**
	 * Blocking reads a packet.
	 * If nonce is specified, waits for a packet with a given nonce. If other packets are received, calls processMessage over them.
	 */
	QJsonObject readMessage(const QString &nonce = QString());

	void sendMessage(const QJsonObject &packet, int opCode = 1);

	/**
 * Processes incoming messages.
 */
	void processMessage(const QJsonObject &msg);

private:
	/// Blocking waits until given amount of bytes is available
	QByteArray blockingReadBytes(int bytes);

	/// Non-blocking processes received messsages
	void readAndProcessMessages();

private:
	QLocalSocket socket_;
	bool isConnected_ = false;
	QString userID_;
	QString cdn_;
	int nonceCounter_ = 0;
	int blockingRead_ = 0;

private:
	QNetworkAccessManager netMgr_;
	QCache<QString, QImage> avatarsCache_;

};

