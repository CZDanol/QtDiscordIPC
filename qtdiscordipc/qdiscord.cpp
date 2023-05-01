#include "qdiscord.h"

#include <QJsonDocument>
#include <QRandomGenerator64>
#include <QFile>
#include <QHttpMultiPart>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QDesktopServices>
#include <QUrlQuery>
#include <QMetaEnum>
#include <QRegularExpression>

struct MessageHeader {
	uint32_t opcode;
	uint32_t length;
};
static_assert(sizeof(MessageHeader) == 8);

double QDiscord::ipcToUIVolume(double v) {
	if(v <= 0)
		return 0;
	else if(v <= 100)
		return 17.362 * log(v) + 20.054;
	else
		return 144.86 * log(v) - 567.21;
}

double QDiscord::uiToIPCVolume(double v) {
	if(v <= 0)
		return 0;
	else if(v <= 100)
		return exp((v - 20.054) / 17.362);
	else
		return exp((v + 567.21) / 144.86);
}

QDiscord::QDiscord() {
	QObject::connect(&socket_, &QLocalSocket::errorOccurred, this, [this](const QLocalSocket::LocalSocketError &err) {
		qWarning() << "QDiscord socket error: " << static_cast<int>(err);
	});
	QObject::connect(&socket_, &QLocalSocket::disconnected, this, [this] {
		qDebug() << "Disconnected";
		if(connectionError_.isEmpty())
			connectionError_ = "DISCONNECTED";
		disconnect();
	});
	QObject::connect(&socket_, &QLocalSocket::readyRead, this, [this] {
		if(blockingRead_)
			return;

		readAndProcessMessages();
		if(socket_.bytesAvailable())
			qDebug() << "ASSERTION FAILED: bytes still available after read and process messages";
	});

	dispatchTimer_.setInterval(1000);
	dispatchTimer_.callOnTimeout(this, &QDiscord::dispatch);
}

QDiscord::~QDiscord() {
	for(const auto r: pendingReplies_)
		delete r;
}

bool QDiscord::connect(const QString &clientID, const QString &clientSecret) {
	connectionError_.clear();
	processing_++;
	const bool r = [&]() {
		if(clientID.isEmpty() || clientSecret.isEmpty()) {
			qDebug() << "Missing client ID or secret";
			connectionError_ = "ERR 0";
			return false;
		}

		// start connecting
		for(int i = 0; i < 10; i++) {
			socket_.connectToServer("discord-ipc-" + QString::number(i));
			qDebug() << "Trying to connect to Discord (" << i << ")";

			if(socket_.waitForConnected(3000))
				break;
		}
		if(socket_.state() != QLocalSocket::ConnectedState) {
			qDebug() << "Connection failed";
			connectionError_ = "ERR 1";
			return false;
		}

		qDebug() << "Connected";
		static const QStringList scopes{"rpc", "identify"};

		// Handshake and dispatch Receive DISPATCH
		{
			sendMessage(QJsonObject{
				{"v",         1},
				{"client_id", clientID},
			}, 0);

			const QDiscordMessage msg = readMessage();

			if(msg.json.isEmpty()) {
				qWarning() << "QDiscord - empty response" << msg.json;
				connectionError_ = "ERR 8";
				return false;
			}


			if(msg.json["cmd"] != "DISPATCH") {
				qWarning() << "QDiscord - unexpected message (expected DISPATCH)" << msg.json["cmd"];
				connectionError_ = "ERR 2";
				return false;
			}

			cdn_ = msg.data["config"]["cdn_host"].toString();
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
			else {
				connectionError_ = "ERR 3";
				qWarning() << "QDiscord Network error (refresh)" << r->errorString();
			}
		}

		// Authenticate from stored token
		if(!oauthData["access_token"].isNull()) {
			sendMessage(QJsonObject{
				{"cmd",   +CommandType::authenticate},
				{"nonce", "auth_0"},
				{"args",  QJsonObject{
					{"access_token", oauthData["access_token"].toString()}
				}},
			});

			const QDiscordMessage msg = readMessage();
			if(msg.json["cmd"] == "AUTHENTICATE" && msg.json["evt"] != "ERROR") {
				qDebug() << "Connected through pre-stored token";
				loadIdentityFromAuth(msg.json);
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
				sendMessage(QJsonObject{
					{"cmd",   +CommandType::authorize},
					{"nonce", "auth_1"},
					{"args",  QJsonObject{
						{"client_id", clientID},
						{"scopes",    QJsonArray::fromStringList(scopes)}
					}},
				});

				const QDiscordMessage msg = readMessage();
				if(msg.json["cmd"] != "AUTHORIZE" || msg.json["evt"] == "ERROR") {
					connectionError_ = "ERR 4";
					qWarning() << "AUTHORIZE ERROR" << msg.json;
					return false;
				}

				authCode = msg.data["code"].toString();
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
					connectionError_ = "ERR 5";
					qWarning() << "QDiscord Network error" << r->errorString();
					return false;
				}

				oauthData = QJsonDocument::fromJson(r->readAll()).object();
			}

			if(oauthData["access_token"].toString().isEmpty()) {
				connectionError_ = "ERR 6";
				qWarning() << "QDiscord failed to obtain access token";
				return false;
			}

			saveOauthData();
		}

		// Authenticate
		{
			sendMessage(QJsonObject{
				{"cmd",   "AUTHENTICATE"},
				{"nonce", "auth_2"},
				{"args",  QJsonObject{
					{"access_token", oauthData["access_token"].toString()}
				}},
			});

			const QDiscordMessage msg = readMessage();
			if(msg.json["cmd"] != "AUTHENTICATE" || msg.json["evt"] == "ERROR") {
				connectionError_ = "ERR 7";
				qWarning() << "AUTHENTICATE ERROR" << msg.json;
				return false;
			}

			loadIdentityFromAuth(msg.json);
		}

		qDebug() << "Connection successful";
		return true;
	}();

	if(!r)
		disconnect();

	else {
		isConnected_ = true;
		dispatchTimer_.start();
		connectionError_.clear();
		emit connected();
	}

	processing_--;
	return r;
}

void QDiscord::disconnect() {
	const bool wasConnected = isConnected_;

	socket_.disconnectFromServer();
	dispatchTimer_.stop();
	isConnected_ = false;
	userID_.clear();

	if(wasConnected)
		emit disconnected();
}

QDiscordReply *QDiscord::sendCommand(const QString &command, const QJsonObject &args, const QJsonObject &msgOverrides) {
	const QString nonce = QStringLiteral("%1:%2").arg(QString::number(nonceCounter_++), QString::number(QRandomGenerator64::global()->generate()));
	QJsonObject message{
		{"cmd",   command},
		{"args",  args},
		{"nonce", nonce}
	};

	for(auto it = msgOverrides.begin(), end = msgOverrides.end(); it != end; it++)
		message[it.key()] = it.value();

	sendMessage(message);

	QDiscordReply *r = new QDiscordReply(nonce);
	pendingReplies_.insert(nonce, r);
	return r;
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

QDiscordMessage QDiscord::readMessage() {
	const QByteArray headerBA = blockingReadBytes(sizeof(MessageHeader));
	if(headerBA.isNull()) {
		qDebug() << "Empty json message";
		return {};
	}

	const MessageHeader &header = *reinterpret_cast<const MessageHeader *>(headerBA.data());

	const QByteArray data = blockingReadBytes(static_cast<int>(header.length));

	QJsonParseError err;
	QDiscordMessage result = QDiscordMessage::fromJson(QJsonDocument::fromJson(data, &err).object(), static_cast<int>(header.opcode));

	if(err.error != QJsonParseError::NoError)
		qWarning() << "QDiscord - failed to parse message\n\n" << data;

	qDebug() << "<<<<< RECV\n" << header.opcode << header.length << result.json << "\n";

	return result;
}

void QDiscord::sendMessage(const QJsonObject &packet, int opCode) {
	const QByteArray payload = QJsonDocument(packet).toJson(QJsonDocument::Compact);

	qDebug() << ">>>>> SEND\n" << opCode << payload.length() << packet << "\n";

	MessageHeader header;
	header.opcode = static_cast<uint32_t>(opCode);
	header.length = static_cast<uint32_t>(payload.length());

	socket_.write(QByteArray::fromRawData(reinterpret_cast<const char *>(&header), sizeof(MessageHeader)) + payload);
}

void QDiscord::processMessage(const QDiscordMessage &msg) {
	if(QDiscordReply *r = pendingReplies_.take(msg.nonce)) {
		emit r->finished(msg);
		r->deleteLater();
		return;
	}

	emit messageReceived(msg);
}

QByteArray QDiscord::blockingReadBytes(int bytes) {
	blockingRead_++;
	processing_++;
	while(socket_.bytesAvailable() < bytes) {
		if(!socket_.waitForReadyRead(10000)) {
			qWarning() << "QDiscord - waitForReadyRead timeout";
			processing_--;
			blockingRead_--;
			return {};
		}
	}
	processing_--;
	blockingRead_--;

	return socket_.read(bytes);
}

void QDiscord::readAndProcessMessages() {
	while(socket_.bytesAvailable())
		processMessage(readMessage());
}

void QDiscord::dispatch() {
/*
 * INVALID command - does nothing
 * const QString nonce = QStringLiteral("%1:%2").arg(QString::number(nonceCounter_++), QString::number(QRandomGenerator64::global()->generate()));
	QJsonObject message{
		{"cmd",   "DISPATCH"},
		{"args",  QJsonArray{}},
		{"nonce", nonce}
	};
	sendMessage(message);*/
}

QString operator +(QDiscord::CommandType ct) {
	static const QHash<int, QString> ht = [] {
		const auto me = QMetaEnum::fromType<QDiscord::CommandType>();
		const auto cnt = me.keyCount();

		QHash<int, QString> r;
		r.reserve(cnt);

		const QRegularExpression regex("([A-Z])");

		for(int i = 0; i < cnt; i++) {
			QString str = me.key(i);
			str.replace(regex, "_\\1"); // Add _ underscore before every capitalized letter (guildStatus -> guild_Status)
			str = str.toUpper(); // Convert all to uppercase (guild_Status -> GUILD_STATUS)

			r.insert(me.value(i), str);
		}

		return r;
	}();
	return ht.value(int(ct));
}
