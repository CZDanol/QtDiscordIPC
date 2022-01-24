#include "qdiscord.h"

#include <QJsonDocument>
#include <QTimer>
#include <QRandomGenerator64>
#include <QFile>
#include <QtNetworkAuth/QOAuth2AuthorizationCodeFlow>
#include <QtNetworkAuth/QOAuthHttpServerReplyHandler>
#include <QHttpMultiPart>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QDesktopServices>
#include <QUrlQuery>
#include <QNetworkAccessManager>
#include <QNetworkReply>

struct MessageHeader {
	uint32_t opcode;
	uint32_t length;
};
static_assert(sizeof(MessageHeader) == 8);

QDiscord::QDiscord() {
	QObject::connect(&socket_, &QLocalSocket::errorOccurred, this, [this](const QLocalSocket::LocalSocketError &err) {
		qWarning() << "QDiscord socket error: " << static_cast<int>(err);
	});
	QObject::connect(&socket_, &QLocalSocket::disconnected, this, [this] {
		disconnect();
	});
	QObject::connect(&socket_, &QLocalSocket::readyRead, this, [this] {
		if(blockingRead_)
			return;

		readAndProcessMessages();
		if(socket_.bytesAvailable())
			qDebug() << "ASSERTION FAILED: bytes still available after read and process messages";
	});
}

bool QDiscord::connect(const QString &clientID, const QString &clientSecret) {
	const bool r = [&]() {
		// start connecting
		socket_.connectToServer("discord-ipc-0");
		qDebug() << "Trying to connect to Discord";

		if(!socket_.waitForConnected(3000)) {
			qDebug() << "Connection failed";
			return false;
		}

		static const QStringList scopes{"rpc", "identify"};

		// Handshake and dispatch Receive DISPATCH
		{
			sendMessage(QJsonObject{
				{"v",         1},
				{"client_id", clientID},
			}, 0);

			const QJsonObject msg = readMessage();

			if(msg["cmd"] != "DISPATCH") {
				qWarning() << "QDiscord - unexpected message (expected DISPATCH)" << msg["cmd"];
				return false;
			}

			cdn_ = msg["data"]["config"]["cdn_host"].toString();
		}

		QJsonObject oauthData;
		QFile oauthFile("discordOauth.json");
		if(oauthFile.exists()) {
			oauthFile.open(QIODevice::ReadOnly);
			oauthData = QJsonDocument::fromJson(oauthFile.readAll()).object();
			oauthFile.close();
		}

		const auto saveOauthData = [&] {
			oauthFile.open(QIODevice::WriteOnly);
			oauthFile.write(QJsonDocument(oauthData).toJson(QJsonDocument::Compact));
			oauthFile.close();
		};

		const auto loadIdentityFromAuth = [&](const QJsonObject &msg) {
			userID_ = msg["data"]["user"]["id"].toString();
		};

		// Try refreshing token
		if(!oauthData["refresh_token"].isNull()) {
			QNetworkAccessManager nm;
			QNetworkRequest req;
			const QUrlQuery q{
				{"client_id",     clientID},
				{"client_secret", clientSecret},
				{"refresh_token", oauthData["refresh_token"].toString()},
				{"scope",         scopes.join(' ')},
				{"grant_type",    "refresh_token"},
			};
			QUrl url("https://discord.com/api/oauth2/token");
			req.setUrl(url);
			req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
			auto r = nm.post(req, q.toString(QUrl::FullyEncoded).toUtf8());

			QEventLoop l;
			QObject::connect(r, &QNetworkReply::finished, &l, &QEventLoop::quit);

			qDebug() << "REFRESH REQ" << req.url() << q.toString();

			l.exec();
			r->deleteLater();

			if(r->error() == QNetworkReply::NoError) {
				qDebug() << "Successfully refreshed token";
				oauthData = QJsonDocument::fromJson(r->readAll()).object();
				saveOauthData();
			}
			else
				qWarning() << "QDiscord Network error (refresh)" << r->errorString();
		}

		// Authenticate from stored token
		if(!oauthData["access_token"].isNull()) {
			const QJsonObject msg = sendCommand("AUTHENTICATE", QJsonObject{
				{"access_token", oauthData["access_token"].toString()}
			});

			if(msg["cmd"] == "AUTHENTICATE") {
				qDebug() << "Connected through pre-stored token";
				loadIdentityFromAuth(msg);
				return true;
			}
		}

		// When we got here, it mens that the automatic authentication on background failed -> start from scratch
		oauthData = {};

		// Send authorization request
		{
			// Authorize in Discord
			QString authCode;
			{
				const QJsonObject msg = sendCommand("AUTHORIZE", QJsonObject{
					{"client_id", clientID},
					{"scopes",    QJsonArray::fromStringList(scopes)}
				});

				if(msg["cmd"] != "AUTHORIZE") {
					qWarning() << "Authorize - unexpected result" << msg;
					return false;
				}

				authCode = msg["data"].toObject()["code"].toString();
			}

			// Get access token
			{
				QNetworkAccessManager nm;
				QNetworkRequest req;
				const QUrlQuery q{
					{"client_id",     clientID},
					{"client_secret", clientSecret},
					{"code",          authCode},
					{"scope",         scopes.join(' ')},
					{"grant_type",    "authorization_code"},
				};
				QUrl url("https://discord.com/api/oauth2/token");
				req.setUrl(url);
				req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
				auto r = nm.post(req, q.toString(QUrl::FullyEncoded).toUtf8());

				QEventLoop l;
				QObject::connect(r, &QNetworkReply::finished, &l, &QEventLoop::quit);

				qDebug() << "AUTH REQ" << req.url() << q.toString();

				l.exec();
				r->deleteLater();

				if(r->error() != QNetworkReply::NoError) {
					qWarning() << "QDiscord Network error" << r->errorString();
					return false;
				}

				oauthData = QJsonDocument::fromJson(r->readAll()).object();
			}

			if(oauthData["access_token"].toString().isEmpty()) {
				qWarning() << "QDiscord failed to obtain access token";
				return false;
			}

			saveOauthData();
		}

		// Authenticate
		{
			const QJsonObject msg = sendCommand("AUTHENTICATE", QJsonObject{
				{"access_token", oauthData["access_token"].toString()}
			});

			if(msg["cmd"] != "AUTHENTICATE") {
				qWarning() << "QDiscord expected AUTHENTICATE";
				return false;
			}

			loadIdentityFromAuth(msg);
		}

		qDebug() << "Connection successful";
		return true;
	}();

	if(!r)
		disconnect();

	isConnected_ = r;

	if(isConnected_) {
		emit connected();
	}

	return r;
}

void QDiscord::disconnect() {
	const bool wasConnected = isConnected_;

	socket_.disconnectFromServer();
	isConnected_ = false;
	userID_.clear();

	if(wasConnected)
		emit disconnected();
}

QJsonObject QDiscord::sendCommand(const QString &command, const QJsonObject &args, const QJsonObject &msgOverrides) {
	const QString nonce = QStringLiteral("%1:%2").arg(QString::number(nonceCounter_++), QString::number(QRandomGenerator64::global()->generate()));
	QJsonObject message{
		{"cmd",   command},
		{"args",  args},
		{"nonce", nonce}
	};

	for(auto it = msgOverrides.begin(), end = msgOverrides.end(); it != end; it++)
		message[it.key()] = it.value();

	sendMessage(message);

	const auto result = readMessage(nonce);
	return std::move(result);
}

QImage QDiscord::getUserAvatar(const QString &userId, const QString &avatarId) {
	if(QImage *img = avatarsCache_.object(avatarId))
		return *img;

	const QString url = QStringLiteral("https://%1/avatars/%2/%3.png").arg(cdn_, userId, avatarId);
	QNetworkReply *r = netMgr_.get(QNetworkRequest(QUrl(url)));
	QObject::connect(r, &QNetworkReply::finished, this, [this, r, avatarId] {
		const QImage img = QImage::fromData(r->readAll());
		r->deleteLater();
		avatarsCache_.insert(avatarId, new QImage(img));
		emit avatarReady(avatarId, img);
	});

	return {};
}

QJsonObject QDiscord::readMessage(const QString &nonce) {
	while(true) {
		const QByteArray headerBA = blockingReadBytes(sizeof(MessageHeader));
		if(headerBA.isNull()) {
			return {};
		}

		const MessageHeader &header = *reinterpret_cast<const MessageHeader *>(headerBA.data());

		const QByteArray data = blockingReadBytes(static_cast<int>(header.length));

		QJsonParseError err;
		QJsonObject result = QJsonDocument::fromJson(data, &err).object();

		if(err.error != QJsonParseError::NoError)
			qWarning() << "QDiscord - failed to parse message\n\n" << data;

		result["opcode"] = static_cast<int>(header.opcode);

		qDebug() << "<<<<< RECV\n" << result << "\n";

		// If the nonce does not match, process the message instead
		if(!nonce.isEmpty() && result["nonce"] != nonce) {
			qDebug() << "Not what wanted, waiting for other messsages";
			processMessage(result);
			continue;
		}

		// If there are any further messages to be processed, process them
		readAndProcessMessages();

		if(result["evt"] == "ERROR")
			return {};

		return result;
	}
}

void QDiscord::sendMessage(const QJsonObject &packet, int opCode) {
	const QByteArray payload = QJsonDocument(packet).toJson(QJsonDocument::Compact);

	qDebug() << ">>>>> SEND\n" << packet << "\n";

	MessageHeader header;
	header.opcode = static_cast<uint32_t>(opCode);
	header.length = static_cast<uint32_t>(payload.length());

	socket_.write(QByteArray::fromRawData(reinterpret_cast<const char *>(&header), sizeof(MessageHeader)) + payload);
}

void QDiscord::processMessage(const QJsonObject &msg) {
	// Delay the event sending so that we don't start processing it for example in the middle of sendCommand
	QTimer::singleShot(0, this, [this, msg] { emit messageReceived(msg); });
}

QByteArray QDiscord::blockingReadBytes(int bytes) {
	blockingRead_++;
	while(socket_.bytesAvailable() < bytes) {
		if(!socket_.waitForReadyRead(30000)) {
			qWarning() << "QDiscord - waitForReadyRead timeout";
			return {};
		}
	}
	blockingRead_--;

	return socket_.read(bytes);
}

void QDiscord::readAndProcessMessages() {
	while(socket_.bytesAvailable()) {
		processMessage(readMessage());
	}
}
